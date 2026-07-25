#pragma once

// Nginx-style master-worker process manager
//
// Master process:
//   - Spawns and monitors worker processes
//   - Handles signals: SIGTERM/SIGINT (shutdown), SIGHUP (reload/upgrade)
//   - Supports dynamic worker count adjustment
//   - Graceful worker replacement: spawn new → stop old
//
// Worker process:
//   - Runs user-provided serve function
//   - Handles SIGTERM for graceful shutdown
//   - Automatically exits when io_context stops
//
// Config is generic: any type with at least `num_workers` and `graceful_timeout` fields.

#include "../detail/config.hpp"
#include "../coroutine/task.hpp"
#include "../io/io_context.hpp"

#include <functional>
#include <cstdint>
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <concepts>
#include <thread>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <memory>

#ifndef ASYNC_NET_WINDOWS
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#else
#include <windows.h>
#endif

namespace async_net {

// ============================================================================
// WorkerConfig concept — any type with num_workers + graceful_timeout
// ============================================================================

template<typename Cfg>
concept WorkerConfig = requires(Cfg c) {
    { c.num_workers } -> std::convertible_to<unsigned int>;
    { c.graceful_timeout } -> std::convertible_to<std::chrono::seconds>;
};

// ============================================================================
// Worker execution mode
// ============================================================================

enum class worker_mode {
    process,  // Multi-process (fork) — Unix only, Windows falls back to thread
    thread,   // Multi-thread (std::thread) — cross-platform
    auto_mode // Auto: process on Unix, thread on Windows
};

// ============================================================================
// Default worker configuration
// ============================================================================

struct default_worker_config {
    unsigned int num_workers = 0;             // 0 = auto-detect (hardware_concurrency)
    std::chrono::seconds graceful_timeout{5}; // Time to wait before force kill/join
    worker_mode mode = worker_mode::auto_mode;
};

// Backward-compatible alias
using worker_config = default_worker_config;

// ============================================================================
// Process manager signal state (non-template, shared across instantiations)
// ============================================================================

namespace detail {

struct pm_signal_state {
    static inline std::atomic<bool> shutdown_requested{false};
    static inline std::atomic<bool> reload_requested{false};
};

} // namespace detail

// ============================================================================
// Process manager — nginx-style master-worker model (generic config)
// ============================================================================

template<WorkerConfig Cfg>
class process_manager {
public:
    // Worker function: receives (worker_id, io_context&, const config&).
    // Should return when the server stops.
    using worker_fn = std::function<Task<void>(int /*worker_id*/, io_context&, const Cfg&)>;

    // Reload callback: called on SIGHUP. Returns new config.
    // If not set, SIGHUP is ignored.
    using reload_fn = std::function<Cfg()>;

    template<typename F>
    explicit process_manager(F&& worker)
        : worker_fn_(std::forward<F>(worker)) {}

    ~process_manager() = default;

    // Set reload callback (called on SIGHUP)
    void on_reload(reload_fn fn) { reload_fn_ = std::move(fn); }

    // Set initial configuration
    void configure(Cfg cfg) {
        config_ = cfg;
        resolve_config(config_);
    }

    // Run the master process (blocks until shutdown).
    // Forks workers, monitors them, handles signals.
    // Returns exit code (0 = normal shutdown).
    int run();

    // Request graceful shutdown (can be called from signal handler)
    void request_shutdown() {
        detail::pm_signal_state::shutdown_requested.store(true, std::memory_order_relaxed);
    }

    // Request reload (can be called from signal handler)
    void request_reload() {
        detail::pm_signal_state::reload_requested.store(true, std::memory_order_relaxed);
    }

private:
    struct worker_info {
#ifndef ASYNC_NET_WINDOWS
        pid_t pid = 0;
#endif
        std::thread thread;  // For thread mode
        std::shared_ptr<std::atomic<bool>> running;  // Shared with worker thread
        int worker_id = 0;
        unsigned int generation = 0;
        bool stopping = false;
        bool is_thread = false;  // true if this worker is a thread, false if process

        // Make movable
        worker_info() : running(std::make_shared<std::atomic<bool>>(false)) {}
        worker_info(worker_info&&) noexcept = default;
        worker_info& operator=(worker_info&&) noexcept = default;
        worker_info(const worker_info&) = delete;
        worker_info& operator=(const worker_info&) = delete;
    };

    static void resolve_config(Cfg& cfg) {
        unsigned int nw = static_cast<unsigned int>(cfg.num_workers);
        if (nw == 0) {
            nw = std::thread::hardware_concurrency();
            if (nw == 0) nw = 4;
            cfg.num_workers = nw;
        }
    }

    // Resolve auto_mode to actual mode based on platform
    static worker_mode resolve_mode(worker_mode m) {
        if (m == worker_mode::auto_mode) {
#ifdef ASYNC_NET_WINDOWS
            return worker_mode::thread;
#else
            return worker_mode::process;
#endif
        }
#ifdef ASYNC_NET_WINDOWS
        // Windows doesn't support fork, fall back to thread
        if (m == worker_mode::process) return worker_mode::thread;
#endif
        return m;
    }

    void spawn_workers_process(unsigned int count, unsigned int generation);
    void spawn_workers_thread(unsigned int count, unsigned int generation);
    void spawn_workers(unsigned int count, unsigned int generation);
    void stop_old_workers(unsigned int keep_generation);
    void stop_all_workers();
    void reap_children();
    void reap_threads();
    void install_signal_handlers();

#ifndef ASYNC_NET_WINDOWS
    static void master_signal_handler(int sig);
    static void worker_signal_handler(int sig);
#endif

    worker_fn worker_fn_;
    reload_fn reload_fn_;
    Cfg config_{};

    std::vector<worker_info> workers_;
    io_context ctx_;
    unsigned int current_generation_ = 0;
};

// ============================================================================
// Implementation (template — must be in header)
// ============================================================================

#ifndef ASYNC_NET_WINDOWS

template<WorkerConfig Cfg>
void process_manager<Cfg>::master_signal_handler(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            detail::pm_signal_state::shutdown_requested.store(true, std::memory_order_relaxed);
            break;
        case SIGHUP:
            detail::pm_signal_state::reload_requested.store(true, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

template<WorkerConfig Cfg>
void process_manager<Cfg>::worker_signal_handler(int sig) {
    (void)sig;
}

template<WorkerConfig Cfg>
void process_manager<Cfg>::install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = master_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, nullptr);
}

#endif // !ASYNC_NET_WINDOWS

// ---------------------------------------------------------------------------
// spawn_workers_process (fork-based)
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::spawn_workers_process(unsigned int count, unsigned int generation) {
#ifdef ASYNC_NET_WINDOWS
    (void)count; (void)generation;
#else
    for (unsigned int i = 0; i < count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[master] fork() failed: " << strerror(errno) << std::endl;
            continue;
        }

        if (pid == 0) {
            // ---- Child process (worker) ----
            struct sigaction sa{};
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGTERM, &sa, nullptr);
            sigaction(SIGINT,  &sa, nullptr);
            sigaction(SIGHUP,  &sa, nullptr);
            sigaction(SIGPIPE, &sa, nullptr);

            sa.sa_handler = worker_signal_handler;
            sigaction(SIGTERM, &sa, nullptr);

            try {
                io_context ctx;
                auto task = worker_fn_(static_cast<int>(i), ctx, config_);
                task.resume();
                ctx.run();
            } catch (const std::exception& e) {
                std::cerr << "[worker " << i << "] exception: " << e.what() << std::endl;
            }
            _exit(0);
        }

        // ---- Parent process (master) ----
        worker_info wi;
        wi.pid = pid;
        wi.worker_id = static_cast<int>(i);
        wi.generation = generation;
        wi.stopping = false;
        wi.is_thread = false;
        workers_.push_back(std::move(wi));

        std::cout << "[master] Spawned worker " << i
                  << " pid=" << pid
                  << " gen=" << generation << std::endl;
    }
#endif
}

// ---------------------------------------------------------------------------
// spawn_workers_thread (std::thread-based)
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::spawn_workers_thread(unsigned int count, unsigned int generation) {
    bool share_ctx = ctx_.backend().supports_concurrent_poll();

    for (unsigned int i = 0; i < count; ++i) {
        worker_info wi;
        wi.worker_id = static_cast<int>(i);
        wi.generation = generation;
        wi.stopping = false;
        wi.is_thread = true;
        wi.running->store(true, std::memory_order_relaxed);

        auto running = wi.running;
        wi.thread = std::thread([this, running, i, share_ctx]() {
            try
            {
                if (share_ctx) {
                    // Concurrent-poll backend (IOCP/kqueue):
                   // Single io_context, single server, multiple threads call ctx_.run().
                   // Only thread 0 creates the server via worker_fn_.
                   // Other threads just call ctx_.run() to distribute completions.
                    if (i== 0) {
                        auto task = worker_fn_(0, ctx_, config_);
                        task.resume();
                        ctx_.run();
                    } else {
                        ctx_.run();
                    }
                } else {
                    io_context ctx2;
                    auto task = worker_fn_(static_cast<int>(i), ctx2, config_);
                    task.resume();
                    ctx2.run();
                }
            } catch (const std::exception& e) {
                std::cerr << "[worker " << i << "] exception: " << e.what() << std::endl;
            }
            running->store(false, std::memory_order_relaxed);
        });
        workers_.push_back(std::move(wi));
        std::cout << "[master] Spawned thread worker " << i << " (ctx.run) gen=" << generation << std::endl;
    }
}

// ---------------------------------------------------------------------------
// spawn_workers (dispatcher)
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::spawn_workers(unsigned int count, unsigned int generation) {
    worker_mode mode = resolve_mode(config_.mode);
    if (mode == worker_mode::thread) {
        spawn_workers_thread(count, generation);
    } else {
        spawn_workers_process(count, generation);
    }
}

// ---------------------------------------------------------------------------
// stop_old_workers
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::stop_old_workers(unsigned int keep_generation) {
    auto timeout = std::chrono::seconds{config_.graceful_timeout};

    // Signal threads/processes to stop
    for (auto& w : workers_) {
        if (w.generation != keep_generation && !w.stopping) {
            std::cout << "[master] Stopping old worker " << w.worker_id
                      << " gen=" << w.generation;
            if (w.is_thread) {
                std::cout << " (thread)" << std::endl;
                w.running->store(false, std::memory_order_relaxed);
                // Stop the io_context to unblock ctx.run()
                ctx_.stop();
            } else {
#ifndef ASYNC_NET_WINDOWS
                std::cout << " pid=" << w.pid << std::endl;
                kill(w.pid, SIGTERM);
#endif
            }
            w.stopping = true;
        }
    }

    // Wait for graceful shutdown
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (auto& w : workers_) {
        if (w.generation != keep_generation && w.stopping) {
            if (w.is_thread) {
                // Thread mode: wait for running flag or join with timeout
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!w.running->load(std::memory_order_relaxed)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (w.thread.joinable()) {
                    w.thread.join();
                }
            } else {
#ifndef ASYNC_NET_WINDOWS
                // Process mode: waitpid with timeout
                while (std::chrono::steady_clock::now() < deadline) {
                    int status;
                    pid_t result = waitpid(w.pid, &status, WNOHANG);
                    if (result == w.pid) { w.pid = 0; break; }
                    if (result < 0 && errno == ECHILD) { w.pid = 0; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (w.pid != 0) {
                    std::cerr << "[master] Force killing worker " << w.worker_id
                              << " pid=" << w.pid << std::endl;
                    kill(w.pid, SIGKILL);
                    waitpid(w.pid, nullptr, 0);
                    w.pid = 0;
                }
#endif
            }
        }
    }

    // Remove old workers
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [keep_generation](const worker_info& w) {
                           return w.generation != keep_generation;
                       }),
        workers_.end()
    );
}

// ---------------------------------------------------------------------------
// stop_all_workers
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::stop_all_workers() {
    std::cout << "[master] Stopping all " << workers_.size() << " workers..." << std::endl;

    // Signal all workers to stop
    bool has_thread_workers = false;
    for (auto& w : workers_) {
        if (!w.stopping) {
            if (w.is_thread) {
                w.running->store(false, std::memory_order_relaxed);
                has_thread_workers = true;
            } else {
#ifndef ASYNC_NET_WINDOWS
                if (w.pid != 0) {
                    kill(w.pid, SIGTERM);
                }
#endif
            }
            w.stopping = true;
        }
    }

    // Stop io_context to unblock ctx.run() in thread workers
    if (has_thread_workers) {
        ctx_.stop();
    }

    // Wait for graceful shutdown
    auto timeout = std::chrono::seconds{config_.graceful_timeout};
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_exited = true;
        for (auto& w : workers_) {
            if (w.is_thread) {
                if (w.running->load(std::memory_order_relaxed)) {
                    all_exited = false;
                }
            } else {
#ifndef ASYNC_NET_WINDOWS
                if (w.pid != 0) {
                    int status;
                    pid_t result = waitpid(w.pid, &status, WNOHANG);
                    if (result == w.pid || (result < 0 && errno == ECHILD)) {
                        w.pid = 0;
                    } else if (w.pid != 0) {
                        all_exited = false;
                    }
                }
#endif
            }
        }
        if (all_exited) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force kill remaining processes / detach threads
    for (auto& w : workers_) {
        if (w.is_thread) {
            if (w.thread.joinable()) {
                w.thread.join();
            }
        } else {
#ifndef ASYNC_NET_WINDOWS
            if (w.pid != 0) {
                std::cerr << "[master] Force killing worker " << w.worker_id
                          << " pid=" << w.pid << std::endl;
                kill(w.pid, SIGKILL);
                waitpid(w.pid, nullptr, 0);
                w.pid = 0;
            }
#endif
        }
    }

    workers_.clear();
}

// ---------------------------------------------------------------------------
// reap_children (process mode only)
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::reap_children() {
#ifndef ASYNC_NET_WINDOWS
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;

        for (auto& w : workers_) {
            if (!w.is_thread && w.pid == pid) {
                std::cout << "[master] Worker " << w.worker_id
                          << " pid=" << pid
                          << " gen=" << w.generation << " exited" << std::endl;
                w.pid = 0;
                break;
            }
        }
    }

    // Remove reaped process workers (keep thread workers)
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [](const worker_info& w) {
                           return !w.is_thread && w.pid == 0;
                       }),
        workers_.end()
    );
#endif
}

// ---------------------------------------------------------------------------
// reap_threads (thread mode only)
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
void process_manager<Cfg>::reap_threads() {
    for (auto& w : workers_) {
        if (w.is_thread && !w.running->load(std::memory_order_relaxed)) {
            std::cout << "[master] Thread worker " << w.worker_id
                      << " gen=" << w.generation << " exited" << std::endl;
            if (w.thread.joinable()) {
                w.thread.join();
            }
        }
    }

    // Remove exited thread workers
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [](const worker_info& w) {
                           return w.is_thread && !w.running->load(std::memory_order_relaxed) && !w.thread.joinable();
                       }),
        workers_.end()
    );
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg>
int process_manager<Cfg>::run() {
    resolve_config(config_);
    worker_mode mode = resolve_mode(config_.mode);

    unsigned int nw = static_cast<unsigned int>(config_.num_workers);
    std::cout << "[master] Starting " << nw << " "
              << (mode == worker_mode::thread ? "thread" : "process")
              << " workers" << std::endl;

#ifndef ASYNC_NET_WINDOWS
    if (mode == worker_mode::process) {
        install_signal_handlers();
    }
#endif

    current_generation_ = 1;
    spawn_workers(nw, current_generation_);

#ifndef ASYNC_NET_WINDOWS
    if (mode == worker_mode::process) {
        std::cout << "[master] All workers running. PID=" << getpid() << std::endl;
        std::cout << "[master] Commands: kill -HUP " << getpid() << " (reload), "
                  << "kill -TERM " << getpid() << " (shutdown)" << std::endl;
    } else
#endif
    {
        std::cout << "[master] All thread workers running. PID="
#ifndef ASYNC_NET_WINDOWS
                  << getpid()
#else
                  << GetCurrentProcessId()
#endif
                  << std::endl;
    }

    // Main event loop
    while (!detail::pm_signal_state::shutdown_requested.load(std::memory_order_relaxed)) {
        // Check for reload request
        if (detail::pm_signal_state::reload_requested.exchange(false, std::memory_order_relaxed)) {
            std::cout << "[master] Reload requested" << std::endl;

            Cfg new_config = config_;
            if (reload_fn_) {
                new_config = reload_fn_();
                if (static_cast<unsigned int>(new_config.num_workers) == 0) {
                    new_config.num_workers = config_.num_workers;
                }
            }

            current_generation_++;
            unsigned int new_nw = static_cast<unsigned int>(new_config.num_workers);
            std::cout << "[master] Spawning generation " << current_generation_
                      << " with " << new_nw << " workers" << std::endl;
            spawn_workers(new_nw, current_generation_);

            stop_old_workers(current_generation_);

            config_.num_workers = new_config.num_workers;
            std::cout << "[master] Reload complete. " << workers_.size()
                      << " active workers" << std::endl;
        }

        // Reap exited workers
        if (mode == worker_mode::thread) {
            reap_threads();
        } else {
            reap_children();
        }

        // Check if all workers have exited
        if (workers_.empty()) {
            std::cerr << "[master] All workers exited. Shutting down." << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[master] Shutdown requested" << std::endl;
    stop_all_workers();
    std::cout << "[master] All workers stopped. Exiting." << std::endl;

    return 0;
}

// ============================================================================
// Convenience: run with master-worker model (generic config)
// ============================================================================

template<WorkerConfig Cfg, typename WorkerFn>
int run_mp_master(
    WorkerFn&& worker,
    Cfg config = {},
    std::function<Cfg()> reload_fn = nullptr
) {
    process_manager<Cfg> pm(std::forward<WorkerFn>(worker));
    pm.configure(config);
    if (reload_fn) {
        pm.on_reload(std::move(reload_fn));
    }
    return pm.run();
}

} // namespace async_net
