#pragma once

#include "executor.hpp"
#include <queue>
#include <mutex>
#include <functional>

namespace async_net {

// ---------------------------------------------------------------------------
// strand — serialized execution on top of any executor
//
// Guarantees that handlers posted through this strand are never executed
// concurrently, even if the underlying executor is multi-threaded.
//
// Similar to boost::asio::strand.
//
// Usage:
//   strand s(my_executor);
//   s.post([]{ /* runs serially */ });
//   s.post([]{ /* runs after the previous one completes */ });
// ---------------------------------------------------------------------------
class strand : public executor {
public:
    explicit strand(executor& inner) : inner_(&inner) {}

    // Post a handler for serialized execution.
    // Thread-safe: may be called from any thread.
    void post(std::function<void()> fn) override {
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
    executor& inner() noexcept { return *inner_; }

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

    executor* inner_;
    std::mutex mutex_;
    std::queue<std::function<void()>> queue_;
    bool running_ = false;  // protected by mutex_
};

} // namespace async_net
