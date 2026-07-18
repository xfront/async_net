#pragma once

#include "executor.hpp"
#include "schedule.hpp"
#include "../coroutine/task.hpp"
#include "../coroutine/spawn.hpp"
#include "../io/io_context.hpp"
#include <thread>
#include <chrono>
#include <concepts>
#include <cstdint>

namespace async_net {

// ---------------------------------------------------------------------------
// run_on — switch execution context within a coroutine
//
// Usage:
//   co_await run_on(pool_executor);  // switch to pool thread
//   co_await run_on(io_ctx);         // switch back to io_context
//
// IMPORTANT: The executor must outlive the coroutine.
// ---------------------------------------------------------------------------
struct run_on {
    executor& ex_;
};

struct RunOnAwaiter {
    executor* ex_;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        ex_->post([h]() mutable { h.resume(); });
    }

    void await_resume() noexcept {}
};

inline RunOnAwaiter operator co_await(run_on r) noexcept {
    return RunOnAwaiter{&r.ex_};
}

// ---------------------------------------------------------------------------
// current_thread_id
// ---------------------------------------------------------------------------
inline std::thread::id current_thread_id() {
    return std::this_thread::get_id();
}

// ---------------------------------------------------------------------------
// co_spawn — start a coroutine on a specific executor
//
// The task does NOT begin until it runs on the executor's thread.
// Returns a JoinHandle<T> that can be co_awaited or detached.
// ---------------------------------------------------------------------------
namespace detail {

// Lazy wrapper: switches to the executor first, then runs the task.
// Because this is a Task<T> (lazy), it only starts when resumed.
// The run_on ensures the task begins on the executor's thread.
template<typename T>
Task<T> on_executor_task(executor* ex, Task<T> task) {
    co_await run_on(*ex);
    co_return co_await std::move(task);
}

} // namespace detail

template<typename T>
JoinHandle<T> co_spawn(executor& ex, Task<T> task) {
    // spawn() wraps the lazy on_executor_task in an eager coroutine.
    // The eager coroutine starts immediately, hits co_await on the lazy task,
    // which triggers: run_on(ex) → suspend → post to executor.
    // The task only starts when resumed on the executor's thread.
    return spawn(detail::on_executor_task(&ex, std::move(task)));
}

// ---------------------------------------------------------------------------
// detach — fire-and-forget a coroutine on a specific executor
// ---------------------------------------------------------------------------
template<typename T>
void detach(executor& ex, Task<T> task) {
    auto jh = co_spawn(ex, std::move(task));
    jh.detach();
}

// ---------------------------------------------------------------------------
// Scheduled execution — Java ScheduledExecutorService style
//
// schedule_once(ctx, delay, task)
//   Run a task once after the given delay.
//   Returns a scheduled_task that can be cancelled.
//
// schedule_at_fixed_rate(ctx, initial_delay, factory, period)
//   Run a task periodically. The factory callable is invoked once per
//   iteration to produce a fresh Task<T>.
//   Returns a scheduled_task that can be cancelled.
//
// Usage:
//   auto t = schedule_once(ctx, 500ms, my_task());
//   t.cancel();
//
//   auto t2 = schedule_at_fixed_rate(ctx, 1s,
//       []{ return my_task(); }, 2s);
//   t2.cancel();  // stops periodic execution
//
// The scheduled_task owns the coroutine frame. cancel() destroys it
// immediately; any pending sleep_for timer detects the destruction via
// a shared alive flag and skips the resume — no crash.
// ---------------------------------------------------------------------------

namespace detail {

// Eager coroutine for one-shot scheduled execution
template<typename T>
Task<void> scheduled_once_task(io_context* c, std::chrono::steady_clock::duration d,
                                Task<T> t) {
    co_await sleep_for(d, *c);
    co_await std::move(t);
}

// Eager coroutine for periodic scheduled execution
template<typename F>
Task<void> scheduled_periodic_task(io_context* c,
                                    std::chrono::steady_clock::duration d,
                                    F factory,
                                    std::chrono::steady_clock::duration iv) {
    co_await sleep_for(d, *c);
    while (true) {
        co_await factory();
        co_await sleep_for(iv, *c);
    }
}

} // namespace detail

// schedule_once — run a task once after a delay
// Returns a scheduled_task (like Java's ScheduledFuture<?>).
template<typename T>
scheduled_task schedule_once(io_context& ctx,
                              std::chrono::steady_clock::duration delay,
                              Task<T> task) {
    auto lazy = detail::scheduled_once_task(&ctx, delay, std::move(task));
    // Create eager wrapper via spawn, then extract the raw handle
    auto jh = spawn(std::move(lazy));
    std::coroutine_handle<> h = jh.release_handle();
    return scheduled_task(h);
}

// schedule_at_fixed_rate — run a task factory periodically
// `factory` is called once per iteration to produce a fresh Task<T>.
// Returns a scheduled_task (like Java's ScheduledFuture<?>).
template<typename F>
    requires std::invocable<F> && requires { std::invoke_result_t<F>(); }
scheduled_task schedule_at_fixed_rate(io_context& ctx,
                                       std::chrono::steady_clock::duration initial_delay,
                                       F task_factory,
                                       std::chrono::steady_clock::duration period) {
    auto lazy = detail::scheduled_periodic_task(&ctx, initial_delay,
                                                 std::move(task_factory), period);
    auto jh = spawn(std::move(lazy));
    std::coroutine_handle<> h = jh.release_handle();
    return scheduled_task(h);
}

// Legacy schedule() — thin wrappers for backward compatibility
template<typename T>
JoinHandle<T> schedule(io_context& ctx,
                        std::chrono::steady_clock::duration delay,
                        Task<T> task) {
    auto delayed = [](io_context* c, std::chrono::steady_clock::duration d,
                      Task<T> t) -> Task<T> {
        co_await sleep_for(d, *c);
        co_return co_await std::move(t);
    };
    return spawn(delayed(&ctx, delay, std::move(task)));
}

template<typename F>
    requires std::invocable<F> && requires { std::invoke_result_t<F>(); }
JoinHandle<void> schedule(io_context& ctx,
                           std::chrono::steady_clock::duration delay,
                           F task_factory,
                           std::chrono::steady_clock::duration interval) {
    auto periodic = [](io_context* c, std::chrono::steady_clock::duration d,
                       F factory, std::chrono::steady_clock::duration iv) -> Task<void> {
        co_await sleep_for(d, *c);
        while (true) {
            co_await factory();
            co_await sleep_for(iv, *c);
        }
    };
    return spawn(periodic(&ctx, delay, std::move(task_factory), interval));
}

} // namespace async_net
