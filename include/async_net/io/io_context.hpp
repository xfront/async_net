#pragma once

#include "io_backend.hpp"
#include "../executor/executor.hpp"
#include "../coroutine/task.hpp"
#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

namespace async_net {

class io_context : public executor {
public:
    io_context();
    explicit io_context(std::unique_ptr<IoBackend> backend);

    // Shared backend mode: multiple io_context instances share the same backend.
    // This enables multi-threaded event loop on Windows (IOCP distributes
    // completions across threads calling GetQueuedCompletionStatus).
    // All io_context instances sharing the backend must have the same lifetime.
    explicit io_context(std::shared_ptr<IoBackend> backend);

    ~io_context();

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    // Run the event loop. Blocks until stop() is called.
    void run();

    // Run the event loop with multiple threads, auto-selecting the best strategy:
    //
    //   supports_concurrent_poll()==true  (epoll / kqueue / IOCP):
    //       All N threads share this io_context. The backend distributes events
    //       across threads (epoll_wait/kevent/GetQueuedCompletionStatus are
    //       thread-safe for the same fd/handle).
    //
    //   supports_concurrent_poll()==false (io_uring):
    //       If worker_factory is provided: creates N per-thread io_context
    //       instances, calling worker_factory(wctx) in each thread to set up
    //       work (e.g. SO_REUSEPORT acceptor) and run the loop.
    //       If no factory: falls back to single-threaded run().
    //
    // Blocks until all threads exit.
    void run_mt(unsigned num_threads = 0,
                std::function<void(io_context&)> worker_factory = nullptr);

    // Run the event loop until all work is done (work_count_ == 0 and no pending).
    // Use with work_guard to control the lifetime:
    //   auto guard = ctx.make_work();
    //   // ... post work, create tasks ...
    //   guard.reset();  // release work
    //   ctx.run_until_complete();  // exits when all work done
    void run_until_complete();

    // Run one iteration of the event loop. Returns number of handlers executed.
    size_t run_one();

    // Poll for ready events without blocking. Returns number of handlers executed.
    size_t poll();

    // Stop the event loop
    void stop();

    // Wake up the backend from poll() (for cross-thread notifications)
    void wake() { if (backend_) backend_->wake(); }

    // Check if stopped
    bool stopped() const { return stopped_.load(std::memory_order_relaxed); }

    // Reset after stop
    void restart() { stopped_.store(false, std::memory_order_relaxed); }

    // Post a function to be executed in the event loop. Thread-safe.
    // Automatically increments work count; the event loop will not exit
    // until this function has been executed.
    void post(std::function<void()> func) override;

    // -----------------------------------------------------------------------
    // Timer / Schedule API
    // -----------------------------------------------------------------------

    // Post a callback to execute after a delay.
    // Thread-safe: may be called from any thread.
    template<typename Rep, typename Period>
    void post_after(std::chrono::duration<Rep, Period> delay, std::function<void()> cb) {
        post_at(std::chrono::steady_clock::now() + delay, std::move(cb));
    }

    // Post a callback to execute at a specific time point.
    // Thread-safe: may be called from any thread.
    void post_at(std::chrono::steady_clock::time_point deadline, std::function<void()> cb);

    // Get the backend
    IoBackend& backend() { return *backend_; }
    const IoBackend& backend() const { return *backend_; }

    // Get the current io_context for this thread (if any)
    static io_context* current() { return current_context_; }

    // -----------------------------------------------------------------------
    // work_guard — prevents run() from exiting while work is in progress
    //
    // Usage:
    //   { auto guard = make_work(); ... }  // work_count_++ on construction
    //                                       // work_count_-- on destruction
    // -----------------------------------------------------------------------
    class work_guard {
    public:
        explicit work_guard(io_context& ctx) noexcept : ctx_(&ctx) {
            ctx_->work_count_.fetch_add(1, std::memory_order_relaxed);
        }

        work_guard(const work_guard& other) noexcept : ctx_(other.ctx_) {
            if (ctx_) ctx_->work_count_.fetch_add(1, std::memory_order_relaxed);
        }

        work_guard(work_guard&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

        work_guard& operator=(const work_guard& other) noexcept {
            if (this != &other) {
                release();
                ctx_ = other.ctx_;
                if (ctx_) ctx_->work_count_.fetch_add(1, std::memory_order_relaxed);
            }
            return *this;
        }

        work_guard& operator=(work_guard&& other) noexcept {
            if (this != &other) {
                release();
                ctx_ = std::exchange(other.ctx_, nullptr);
            }
            return *this;
        }

        ~work_guard() { release(); }

        void reset() noexcept { release(); }

    private:
        void release() noexcept {
            if (ctx_) {
                ctx_->work_count_.fetch_sub(1, std::memory_order_relaxed);
                ctx_->wake();  // Wake up poll() so it can check work_count_
                ctx_ = nullptr;
            }
        }

        io_context* ctx_;
    };

    // Create a work_guard for this io_context
    work_guard make_work() { return work_guard(*this); }

    // Get the current work count (for diagnostics)
    size_t work_count() const noexcept { return work_count_.load(std::memory_order_relaxed); }

private:
    void execute_pending();
    void process_timers();
    int next_timer_timeout_ms() const;

    // Timer entry: (deadline, callback)
    struct TimerEntry {
        std::chrono::steady_clock::time_point deadline;
        std::function<void()> callback;
        bool operator>(const TimerEntry& o) const { return deadline > o.deadline; }
    };

    std::shared_ptr<IoBackend> backend_;
    std::atomic<bool> stopped_{false};
    std::atomic<size_t> work_count_{0};
    size_t handlers_executed_ = 0;

    // Pending function queue
    std::queue<std::function<void()>> pending_;
    std::mutex pending_mutex_;

    // Timer queue (min-heap by deadline)
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> timers_;
    mutable std::mutex timers_mutex_;

    // Thread-local current context
    static thread_local io_context* current_context_;
};

} // namespace async_net
