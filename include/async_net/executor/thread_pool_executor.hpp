#pragma once

#include "executor.hpp"
#include "../detail/thread_pool.hpp"
#include <memory>

namespace async_net {

// ---------------------------------------------------------------------------
// thread_pool_executor — executor that dispatches work to a thread_pool
//
// The thread_pool must outlive this executor (or be owned via shared_ptr).
// ---------------------------------------------------------------------------
class thread_pool_executor : public executor {
public:
    explicit thread_pool_executor(thread_pool& pool) : pool_(&pool) {}

    void post(std::function<void()> fn) override {
        pool_->post(std::move(fn));
    }

    thread_pool& pool() noexcept { return *pool_; }

private:
    thread_pool* pool_;
};

} // namespace async_net
