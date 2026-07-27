#pragma once

// ---------------------------------------------------------------------------
// reactor — Reactor / Dispatcher pattern
//
// Thin wrapper around io_context providing a higher-level event dispatch API.
// Manages timer scheduling, event posting, and the event loop lifecycle.
//
// Usage:
//   network::reactor r(ctx);
//   r.schedule(std::chrono::seconds(5), [] { std::cout << "tick\n"; });
//   r.run();
// ---------------------------------------------------------------------------

#include <async_net/io/io_context.hpp>
#include <functional>
#include <chrono>

namespace async_net::network {

class reactor {
public:
    explicit reactor(io_context& ctx) : ctx_(&ctx) {}

    /// Schedule a callback after a delay (thread-safe).
    template<typename Rep, typename Period>
    void schedule(std::chrono::duration<Rep, Period> delay, std::function<void()> cb) {
        ctx_->post_after(delay, std::move(cb));
    }

    /// Post a callback to the event loop (thread-safe).
    void post(std::function<void()> cb) { ctx_->post_impl(std::move(cb)); }

    /// Run the event loop (blocks until stop()).
    void run() { ctx_->run(); }

    /// Run until stop() or all work is done.
    void run_until_complete() { ctx_->run_until_complete(); }

    /// Stop the event loop.
    void stop() { ctx_->stop(); }

    /// Wake the event loop from poll() (for cross-thread notifications).
    void wake() { ctx_->wake(); }

    /// Access the underlying io_context.
    io_context& get_io_context() { return *ctx_; }
    const io_context& get_io_context() const { return *ctx_; }

private:
    io_context* ctx_;
};

} // namespace async_net::network
