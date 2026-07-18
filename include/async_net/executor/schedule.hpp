#pragma once

#include "../io/io_context.hpp"
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>

namespace async_net {

// ---------------------------------------------------------------------------
// sleep_for — suspend the current coroutine for a duration
//
// Must be co_awaited inside a coroutine running on an io_context event loop.
//
// Usage:
//   co_await sleep_for(std::chrono::seconds(1));
//   co_await sleep_for(std::chrono::milliseconds(500), ctx);  // explicit ctx
//
// The timer is registered with io_context's timer queue. When the deadline
// expires, the coroutine is resumed on the io_context's event loop thread.
//
// Safety: if the coroutine frame is destroyed while sleeping (e.g. via
// scheduled_task::cancel()), the timer callback detects the destruction
// through a shared alive flag and skips the resume — no dangling handle.
// ---------------------------------------------------------------------------

namespace detail {

struct SleepAwaiter {
    std::chrono::steady_clock::time_point deadline;
    io_context* ctx;
    std::optional<io_context::work_guard> guard;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    bool await_ready() const noexcept {
        return std::chrono::steady_clock::now() >= deadline;
    }

    void await_suspend(std::coroutine_handle<> h) {
        if (!ctx) {
            throw std::runtime_error(
                "sleep_for: no io_context available. "
                "Pass an explicit io_context& or run inside io_context::run().");
        }
        guard.emplace(*ctx);  // acquire work guard
        auto alive_copy = alive;  // shared with ~SleepAwaiter
        ctx->post_at(deadline, [h, g = std::move(guard), alive_copy]() mutable {
            g.reset();  // release work guard
            if (*alive_copy) {
                h.resume();
            }
        });
    }

    void await_resume() noexcept {}

    ~SleepAwaiter() {
        *alive = false;  // tell timer callback not to resume
    }
};

} // namespace detail

// sleep_for with explicit io_context
template<typename Rep, typename Period>
detail::SleepAwaiter sleep_for(std::chrono::duration<Rep, Period> duration,
                                io_context& ctx) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    return {deadline, &ctx};
}

// sleep_for using io_context::current()
template<typename Rep, typename Period>
detail::SleepAwaiter sleep_for(std::chrono::duration<Rep, Period> duration) {
    auto deadline = std::chrono::steady_clock::now() + duration;
    return {deadline, io_context::current()};
}

// sleep_until with explicit io_context
inline detail::SleepAwaiter sleep_until(std::chrono::steady_clock::time_point tp,
                                         io_context& ctx) {
    return {tp, &ctx};
}

// sleep_until using io_context::current()
inline detail::SleepAwaiter sleep_until(std::chrono::steady_clock::time_point tp) {
    return {tp, io_context::current()};
}

// ---------------------------------------------------------------------------
// scheduled_task — cancellable handle for scheduled/periodic tasks
//
// Similar to Java's ScheduledFuture<?> returned by scheduleAtFixedRate().
//
// Usage:
//   auto task = schedule_at_fixed_rate(ctx, 1s, []{ return my_task(); }, 2s);
//   // ... later ...
//   task.cancel();   // stops the periodic execution
//   bool done = task.is_cancelled();
// ---------------------------------------------------------------------------
class scheduled_task {
public:
    scheduled_task() = default;

    explicit scheduled_task(std::coroutine_handle<> h)
        : handle_(h) {}

    scheduled_task(scheduled_task&& o) noexcept
        : handle_(std::exchange(o.handle_, nullptr)) {}

    scheduled_task& operator=(scheduled_task&& o) noexcept {
        if (this != &o) {
            destroy();
            handle_ = std::exchange(o.handle_, nullptr);
        }
        return *this;
    }

    scheduled_task(const scheduled_task&) = delete;
    scheduled_task& operator=(const scheduled_task&) = delete;

    ~scheduled_task() { destroy(); }

    // Cancel the scheduled task. Destroys the coroutine frame immediately.
    // Any pending sleep_for timer will detect the destruction via the shared
    // alive flag and skip the resume — no crash.
    void cancel() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    bool is_cancelled() const noexcept { return handle_ == nullptr; }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    std::coroutine_handle<> handle_ = nullptr;
};

} // namespace async_net
