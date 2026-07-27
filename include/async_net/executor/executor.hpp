#pragma once

#include <functional>
#include <memory>

namespace async_net {

// ---------------------------------------------------------------------------
// executor — abstract interface for scheduling work
//
// Any type that implements post() can serve as an executor.
// Concrete implementations: io_context, thread_pool_executor, strand.
// ---------------------------------------------------------------------------
class executor {
public:
    virtual ~executor() = default;

    // Post a callable for asynchronous execution on this executor.
    // Thread-safe: may be called from any thread.
    virtual void post(std::function<void()> fn) = 0;
};

// ---------------------------------------------------------------------------
// ExecutorBase<Derived> — CRTP base for executors ()
//
/// Eliminates virtual dispatch overhead by providing a final override of
// post() that forwards to static_cast<Derived*>(this)->post_impl().
//
/// Concrete executors should:
///   1. Inherit from ExecutorBase<Concrete> instead of executor directly
///   2. Provide a public (non-virtual) post_impl() method
//
/// Usage:
///   class my_executor : public ExecutorBase<my_executor> {
///   public:
///       void post_impl(std::function<void()> fn) { /* ... */ }
///   };
// ---------------------------------------------------------------------------
template<typename Derived>
class ExecutorBase : public executor {
public:
    // Final override — prevents further virtual dispatch.
    // The compiler can devirtualize calls through a Derived* or Derived&.
    void post(std::function<void()> fn) final {
        static_cast<Derived*>(this)->post_impl(std::move(fn));
    }
};

// ---------------------------------------------------------------------------
// any_executor — type-erased executor wrapper with shared ownership
//
// Useful when you need to store or pass an executor with value semantics
// and shared lifetime.
// ---------------------------------------------------------------------------
class any_executor : public executor {
public:
    any_executor() = default;

    explicit any_executor(std::shared_ptr<executor> impl)
        : impl_(std::move(impl)) {}

    // Construct from any executor type (wraps in shared_ptr).
    // The executor must be heap-allocated and outlive this wrapper
    // OR be managed by shared_ptr.
    template<typename E>
    explicit any_executor(std::shared_ptr<E> impl)
        : impl_(std::move(impl)) {}

    void post(std::function<void()> fn) override {
        if (impl_) impl_->post(std::move(fn));
    }

    explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    std::shared_ptr<executor> impl_;
};

} // namespace async_net
