#pragma once

// Nginx-style master-worker process manager — policy-based design
//
// Architecture (inspired by Andrei Alexandrescu's "Modern C++ Design"):
//   - WorkerPolicy<Cfg>: compile-time strategy for spawn/stop/monitor workers
//   - ForkPolicy<Cfg>:   fork() + SIGTERM + waitpid (Unix)
//   - ThreadPolicy<Cfg>: std::thread + atomic flag + ctx.stop() (cross-platform)
//   - AutoPolicy<Cfg>:   selects Fork on Unix, Thread on Windows
//
// The host class process_manager<Cfg, Policy> inherits from the policy (mixin),
// gaining spawn/stop/monitor behavior at compile time — zero runtime dispatch.
//
// Config is generic: any type satisfying WorkerConfig concept.

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
#include <algorithm>

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
// WorkerConfig concept
// ============================================================================

template<typename Cfg>
concept WorkerConfig = requires(Cfg c) {
    { c.num_workers } -> std::convertible_to<unsigned int>;
    { c.graceful_timeout } -> std::convertible_to<std::chrono::seconds>;
};

// ============================================================================
// Worker execution mode (kept for backward compatibility)
// ============================================================================

enum class worker_mode {
    process,  // Multi-process (fork) — Unix only
    thread,   // Multi-thread (std::thread) — cross-platform
    auto_mode // Auto: process on Unix, thread on Windows
};

// ============================================================================
// Default configuration
// ============================================================================

struct default_worker_config {
    unsigned int num_workers = 0;             // 0 = auto-detect (hardware_concurrency)
    std::chrono::seconds graceful_timeout{5}; // Time to wait before force kill/join
    worker_mode mode = worker_mode::auto_mode; // Used by run_mp_master() runtime dispatch
};

using worker_config = default_worker_config;

// ============================================================================
// Signal state (shared across all instantiations)
// ============================================================================

namespace detail {

struct pm_signal_state {
    static inline std::atomic<bool> shutdown_requested{false};
    static inline std::atomic<bool> reload_requested{false};
};

} // namespace detail

// ============================================================================
// worker_entry — per-worker metadata stored by the host
// ============================================================================

template<typename Handle>
struct worker_entry {
    Handle handle{};
    int worker_id = 0;
    unsigned int generation = 0;
    bool stopping = false;

    worker_entry() = default;
    worker_entry(worker_entry&&) noexcept = default;
    worker_entry& operator=(worker_entry&&) noexcept = default;
    worker_entry(const worker_entry&) = delete;
    worker_entry& operator=(const worker_entry&) = delete;
};

// ============================================================================
// WorkerPolicy concept — defines the policy interface
// ============================================================================
// A WorkerPolicy<Cfg> must provide:
//   using handle_type
//   using worker_fn
//   static constexpr const char* name
//   handle_type spawn(int id, worker_fn& fn, const Cfg& cfg, io_context& shared_ctx)
//   void signal_stop(handle_type& h, io_context& shared_ctx)
//   void force_stop(handle_type& h)
//   bool is_alive(handle_type& h)
//   void reap(std::vector<worker_entry<handle_type>>& workers)
//   void install_signals()   [optional]
//   static constexpr bool needs_signals

template<typename P, typename Cfg>
concept WorkerPolicyConcept = requires(P p,
                                       int id,
                                       typename P::worker_fn& fn,
                                       const Cfg& cfg,
                                       io_context& ctx,
                                       typename P::handle_type& h,
                                       std::vector<worker_entry<typename P::handle_type>>& entries) {
    typename P::handle_type;
    typename P::worker_fn;
    { P::name } -> std::convertible_to<const char*>;
    { p.spawn(id, fn, cfg, ctx) } -> std::same_as<typename P::handle_type>;
    { p.signal_stop(h, ctx) };
    { p.force_stop(h) };
    { p.is_alive(h) } -> std::convertible_to<bool>;
    { p.reap(entries) };
};

// ============================================================================
// ForkPolicy — fork() based worker management (Unix only)
// ============================================================================

template<typename Cfg>
struct ForkPolicy {
    using worker_fn = std::function<Task<void>(int, io_context&, const Cfg&)>;

    struct handle_type {
#ifndef ASYNC_NET_WINDOWS
        pid_t pid = 0;
#endif
    };

    static constexpr const char* name = "process";
    static constexpr bool needs_signals = true;

    handle_type spawn(int id, worker_fn& fn, const Cfg& cfg, io_context&) {
        handle_type h;
#ifndef ASYNC_NET_WINDOWS
        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[master] fork() failed: " << strerror(errno) << std::endl;
            return h;
        }

        if (pid == 0) {
            // ---- Child process (worker) ----
            // Reset signal handlers to default
            struct sigaction sa{};
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGTERM, &sa, nullptr);
            sigaction(SIGINT,  &sa, nullptr);
            sigaction(SIGHUP,  &sa, nullptr);
            sigaction(SIGPIPE, &sa, nullptr);

            // Install graceful SIGTERM handler (no-op, just prevents default kill)
            sa.sa_handler = [](int) {};
            sigaction(SIGTERM, &sa, nullptr);

            try {
                io_context ctx;
                auto task = fn(id, ctx, cfg);
                task.resume();
                ctx.run();
            } catch (const std::exception& e) {
                std::cerr << "[worker " << id << "] exception: " << e.what() << std::endl;
            }
            _exit(0);
        }

        // ---- Parent process (master) ----
        h.pid = pid;
        std::cout << "[master] Spawned worker " << id << " pid=" << pid << std::endl;
#else
        (void)id; (void)fn; (void)cfg;
        std::cerr << "[master] ForkPolicy not supported on Windows" << std::endl;
#endif
        return h;
    }

    void signal_stop(handle_type& h, io_context&) {
#ifndef ASYNC_NET_WINDOWS
        if (h.pid != 0) {
            kill(h.pid, SIGTERM);
        }
#else
        (void)h;
#endif
    }

    void force_stop(handle_type& h) {
#ifndef ASYNC_NET_WINDOWS
        if (h.pid != 0) {
            std::cerr << "[master] Force killing worker pid=" << h.pid << std::endl;
            kill(h.pid, SIGKILL);
            waitpid(h.pid, nullptr, 0);
            h.pid = 0;
        }
#else
        (void)h;
#endif
    }

    bool is_alive(handle_type& h) {
#ifndef ASYNC_NET_WINDOWS
        if (h.pid == 0) return false;
        int status;
        pid_t result = waitpid(h.pid, &status, WNOHANG);
        if (result == h.pid || (result < 0 && errno == ECHILD)) {
            h.pid = 0;
            return false;
        }
        return true;
#else
        (void)h;
        return false;
#endif
    }

    void reap(std::vector<worker_entry<handle_type>>& workers) {
#ifndef ASYNC_NET_WINDOWS
        while (true) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) break;

            for (auto& w : workers) {
                if (w.handle.pid == pid) {
                    std::cout << "[master] Worker " << w.worker_id
                              << " pid=" << pid
                              << " gen=" << w.generation << " exited" << std::endl;
                    w.handle.pid = 0;
                    break;
                }
            }
        }

        // Remove reaped workers
        workers.erase(
            std::remove_if(workers.begin(), workers.end(),
                           [](const worker_entry<handle_type>& w) {
                               return w.handle.pid == 0;
                           }),
            workers.end()
        );
#else
        (void)workers;
#endif
    }

    void install_signals() {
#ifndef ASYNC_NET_WINDOWS
        struct sigaction sa{};
        sa.sa_handler = master_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGHUP,  &sa, nullptr);

        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, nullptr);
#endif
    }

private:
#ifndef ASYNC_NET_WINDOWS
    static void master_signal_handler(int sig) {
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
#endif
};

// ============================================================================
// ThreadPolicy — std::thread based worker management (cross-platform)
// ============================================================================

template<typename Cfg>
struct ThreadPolicy {
    using worker_fn = std::function<Task<void>(int, io_context&, const Cfg&)>;

    struct handle_type {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> running;

        handle_type() : running(std::make_shared<std::atomic<bool>>(false)) {}
        handle_type(handle_type&&) noexcept = default;
        handle_type& operator=(handle_type&&) noexcept = default;
        handle_type(const handle_type&) = delete;
        handle_type& operator=(const handle_type&) = delete;
    };

    static constexpr const char* name = "thread";
    static constexpr bool needs_signals = false;

    handle_type spawn(int id, worker_fn& fn, const Cfg& cfg, io_context& shared_ctx) {
        handle_type h;
        h.running->store(true, std::memory_order_relaxed);

        bool share_ctx = shared_ctx.backend().supports_concurrent_poll();
        auto running = h.running;

        h.thread = std::thread([&shared_ctx, &fn, &cfg, running, id, share_ctx]() {
            try {
                if (share_ctx) {
                    // Concurrent-poll backend (IOCP/kqueue):
                    // Single io_context, single server, multiple threads call ctx.run().
                    // Only thread 0 creates the server via worker_fn.
                    if (id == 0) {
                        auto task = fn(0, shared_ctx, cfg);
                        task.resume();
                        shared_ctx.run();
                    } else {
                        shared_ctx.run();
                    }
                } else {
                    // Non-concurrent backend (epoll/io_uring):
                    // Each thread has its own io_context + server (SO_REUSEPORT).
                    io_context ctx2;
                    auto task = fn(id, ctx2, cfg);
                    task.resume();
                    ctx2.run();
                }
            } catch (const std::exception& e) {
                std::cerr << "[worker " << id << "] exception: " << e.what() << std::endl;
            }
            running->store(false, std::memory_order_relaxed);
        });

        std::cout << "[master] Spawned thread worker " << id << std::endl;
        return h;
    }

    void signal_stop(handle_type& h, io_context& shared_ctx) {
        h.running->store(false, std::memory_order_relaxed);
        // Stop the io_context to unblock ctx.run()
        shared_ctx.stop();
    }

    void force_stop(handle_type& h) {
        if (h.thread.joinable()) {
            h.thread.join();
        }
    }

    bool is_alive(handle_type& h) {
        return h.running->load(std::memory_order_relaxed);
    }

    void reap(std::vector<worker_entry<handle_type>>& workers) {
        for (auto& w : workers) {
            if (!w.handle.running->load(std::memory_order_relaxed)) {
                std::cout << "[master] Thread worker " << w.worker_id
                          << " gen=" << w.generation << " exited" << std::endl;
                if (w.handle.thread.joinable()) {
                    w.handle.thread.join();
                }
            }
        }

        // Remove exited workers
        workers.erase(
            std::remove_if(workers.begin(), workers.end(),
                           [](const worker_entry<handle_type>& w) {
                               return !w.handle.running->load(std::memory_order_relaxed)
                                   && !w.handle.thread.joinable();
                           }),
            workers.end()
        );
    }

    void install_signals() {
        // Thread mode: no POSIX signal handling needed
    }
};

// ============================================================================
// AutoPolicy — compile-time platform selection
// ============================================================================

template<typename Cfg>
#ifdef ASYNC_NET_WINDOWS
using AutoPolicy = ThreadPolicy<Cfg>;
#else
using AutoPolicy = ForkPolicy<Cfg>;
#endif

// ============================================================================
// process_manager — host class
// ============================================================================
// Template parameters:
//   Cfg:    configuration type (must satisfy WorkerConfig)
//   Policy: worker lifecycle policy (ForkPolicy / ThreadPolicy / AutoPolicy)
//
// The host inherits from Policy (mixin), gaining spawn/stop/monitor at compile time.

template<WorkerConfig Cfg, typename Policy>
class process_manager : private Policy {
public:
    using handle_type = typename Policy::handle_type;
    using worker_fn = typename Policy::worker_fn;
    using reload_fn = std::function<Cfg()>;
    using entry_type = worker_entry<handle_type>;

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
    static void resolve_config(Cfg& cfg) {
        unsigned int nw = static_cast<unsigned int>(cfg.num_workers);
        if (nw == 0) {
            nw = std::thread::hardware_concurrency();
            if (nw == 0) nw = 4;
            cfg.num_workers = nw;
        }
    }

    void spawn_workers(unsigned int count, unsigned int generation);
    void stop_old_workers(unsigned int keep_generation);
    void stop_all_workers();

    worker_fn worker_fn_;
    reload_fn reload_fn_;
    Cfg config_{};

    std::vector<entry_type> workers_;
    io_context ctx_;
    unsigned int current_generation_ = 0;
};

// ============================================================================
// Implementation
// ============================================================================

// ---------------------------------------------------------------------------
// spawn_workers
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg, typename Policy>
void process_manager<Cfg, Policy>::spawn_workers(unsigned int count, unsigned int generation) {
    for (unsigned int i = 0; i < count; ++i) {
        entry_type entry;
        entry.worker_id = static_cast<int>(i);
        entry.generation = generation;
        entry.stopping = false;
        entry.handle = Policy::spawn(static_cast<int>(i), worker_fn_, config_, ctx_);
        workers_.push_back(std::move(entry));
    }
    std::cout << "[master] Spawned " << count << " " << Policy::name
              << " workers gen=" << generation << std::endl;
}

// ---------------------------------------------------------------------------
// stop_old_workers
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg, typename Policy>
void process_manager<Cfg, Policy>::stop_old_workers(unsigned int keep_generation) {
    auto timeout = std::chrono::seconds{config_.graceful_timeout};

    // Signal old workers to stop
    for (auto& w : workers_) {
        if (w.generation != keep_generation && !w.stopping) {
            std::cout << "[master] Stopping old worker " << w.worker_id
                      << " gen=" << w.generation << std::endl;
            Policy::signal_stop(w.handle, ctx_);
            w.stopping = true;
        }
    }

    // Wait for graceful shutdown
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (auto& w : workers_) {
        if (w.generation != keep_generation && w.stopping) {
            while (std::chrono::steady_clock::now() < deadline) {
                if (!Policy::is_alive(w.handle)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Force stop if still alive
            if (Policy::is_alive(w.handle)) {
                Policy::force_stop(w.handle);
            }
        }
    }

    // Remove old workers
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [keep_generation](const entry_type& w) {
                           return w.generation != keep_generation;
                       }),
        workers_.end()
    );
}

// ---------------------------------------------------------------------------
// stop_all_workers
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg, typename Policy>
void process_manager<Cfg, Policy>::stop_all_workers() {
    std::cout << "[master] Stopping all " << workers_.size() << " workers..." << std::endl;

    // Signal all workers to stop
    for (auto& w : workers_) {
        if (!w.stopping) {
            Policy::signal_stop(w.handle, ctx_);
            w.stopping = true;
        }
    }

    // Wait for graceful shutdown
    auto timeout = std::chrono::seconds{config_.graceful_timeout};
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_exited = true;
        for (auto& w : workers_) {
            if (Policy::is_alive(w.handle)) {
                all_exited = false;
                break;
            }
        }
        if (all_exited) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force stop remaining
    for (auto& w : workers_) {
        if (Policy::is_alive(w.handle)) {
            Policy::force_stop(w.handle);
        }
    }

    workers_.clear();
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

template<WorkerConfig Cfg, typename Policy>
int process_manager<Cfg, Policy>::run() {
    resolve_config(config_);

    unsigned int nw = static_cast<unsigned int>(config_.num_workers);
    std::cout << "[master] Starting " << nw << " " << Policy::name << " workers" << std::endl;

    // Install signal handlers (policy-specific)
    Policy::install_signals();

    current_generation_ = 1;
    spawn_workers(nw, current_generation_);

    std::cout << "[master] All workers running. PID="
#ifndef ASYNC_NET_WINDOWS
              << getpid()
#else
              << GetCurrentProcessId()
#endif
              << std::endl;

    if constexpr (Policy::needs_signals) {
#ifndef ASYNC_NET_WINDOWS
        std::cout << "[master] Commands: kill -HUP " << getpid() << " (reload), "
                  << "kill -TERM " << getpid() << " (shutdown)" << std::endl;
#endif
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

        // Reap exited workers (policy-specific)
        Policy::reap(workers_);

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
// Convenience functions
// ============================================================================

// run_mp_master — auto-select policy based on platform (backward compatible)
// If config has a `mode` field, uses it for runtime dispatch.
template<WorkerConfig Cfg, typename WorkerFn>
int run_mp_master(
    WorkerFn&& worker,
    Cfg config = {},
    std::function<Cfg()> reload_fn = nullptr
) {
    // Runtime dispatch for backward compatibility with worker_mode
    worker_mode mode = worker_mode::auto_mode;
    if constexpr (requires { config.mode; }) {
        mode = config.mode;
    }

    // Resolve auto_mode
    if (mode == worker_mode::auto_mode) {
#ifdef ASYNC_NET_WINDOWS
        mode = worker_mode::thread;
#else
        mode = worker_mode::process;
#endif
    }
#ifdef ASYNC_NET_WINDOWS
    if (mode == worker_mode::process) mode = worker_mode::thread;
#endif

    if (mode == worker_mode::thread) {
        return run_mp_master_thread(std::move(worker), std::move(config), reload_fn);
    } else {
        return run_mp_master_fork(std::move(worker), std::move(config), reload_fn);
    }
}

// run_mp_master_thread — explicit thread policy (compile-time, zero dispatch)
template<WorkerConfig Cfg, typename WorkerFn>
int run_mp_master_thread(
    WorkerFn&& worker,
    Cfg config = {},
    std::function<Cfg()> reload_fn = nullptr
) {
    process_manager<Cfg, ThreadPolicy<Cfg>> pm(std::forward<WorkerFn>(worker));
    pm.configure(config);
    if (reload_fn) pm.on_reload(std::move(reload_fn));
    return pm.run();
}

// run_mp_master_fork — explicit fork policy (compile-time, zero dispatch, Unix only)
template<WorkerConfig Cfg, typename WorkerFn>
int run_mp_master_fork(
    WorkerFn&& worker,
    Cfg config = {},
    std::function<Cfg()> reload_fn = nullptr
) {
    process_manager<Cfg, ForkPolicy<Cfg>> pm(std::forward<WorkerFn>(worker));
    pm.configure(config);
    if (reload_fn) pm.on_reload(std::move(reload_fn));
    return pm.run();
}

} // namespace async_net
