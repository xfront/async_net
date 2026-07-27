#pragma once

#include "executor.hpp"
#include <queue>
#include <mutex>
#include <functional>

namespace async_net {

// ---------------------------------------------------------------------------
// basic_strand<E> — serialized execution on top of a concrete executor
//
/// Guarantees that handlers posted through this strand are never executed
/// concurrently, even if the underlying executor is multi-threaded.
///
/// E is the concrete executor type (e.g. io_context, thread_pool_executor).
/// When E is a concrete type, calls to the inner executor avoid virtual
/// dispatch overhead.
///
/// Usage:
///   basic_strand<io_context> s(my_io_context);
///   s.post([]{ /* runs serially */ });
///   s.post([]{ /* runs after the previous one completes */ });
// ---------------------------------------------------------------------------
template<typename E>
class basic_strand : public ExecutorBase<basic_strand<E>> {
public:
    explicit basic_strand(E& inner) : inner_(&inner) {}

    void post_impl(std::function<void()> fn) {
        bool should_dispatch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(fn));
            should_dispatch = !running_;
            running_ = true;
        }
        if (should_dispatch) {
            inner_->post([this] { run(); });
        }
    }

    // Get the underlying executor
    E& inner() noexcept { return *inner_; }

private:
    void run() {
        while (true) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    running_ = false;
                    return;
                }
                fn = std::move(queue_.front());
                queue_.pop();
            }
            fn();
        }
    }

    E* inner_;
    std::mutex mutex_;
    std::queue<std::function<void()>> queue_;
    bool running_ = false;  // protected by mutex_
};

// ---------------------------------------------------------------------------
// strand — type-erased strand using runtime dispatch (backward compatible)
//
/// Wraps any executor via the virtual executor interface.
/// Use basic_strand<Concrete> for zero-overhead when the executor type
/// is known at compile time.
// ---------------------------------------------------------------------------
using strand = basic_strand<executor>;

} // namespace async_net
