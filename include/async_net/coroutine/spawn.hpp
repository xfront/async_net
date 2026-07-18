#pragma once

#include "task.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <variant>
#include <type_traits>

namespace async_net {

template<typename T> class JoinHandle;

namespace detail {

// ---------------------------------------------------------------------------
// SpawnState — shared between the spawned coroutine's promise and JoinHandle
//
// Locking strategy (single-threaded coroutine model):
//   • `done` is atomic — is_finished() / await_ready() are lock-free.
//   • `mutex` protects ONLY the `waiter` handoff between:
//       - Awaiter::await_suspend  (sets waiter)
//       - FinalAwaiter            (reads & clears waiter)
//       - SpawnPromise destructor (clears waiter on outer coroutine destroy)
// ---------------------------------------------------------------------------
template<typename T>
struct SpawnState {
    std::atomic<bool> done{false};
    std::mutex mutex;                         // protects `waiter` only
    std::coroutine_handle<> waiter = nullptr;
    std::variant<std::monostate, T, std::exception_ptr> result;
};

template<>
struct SpawnState<void> {
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::coroutine_handle<> waiter = nullptr;
    std::exception_ptr exception;
    bool has_value = false;
};

// Thread-local: passes state to promise constructor during coroutine creation.
// Set by spawn(), read by SpawnPromise ctor, cleared immediately after.
template<typename T>
inline thread_local std::shared_ptr<SpawnState<T>>* tl_state_ptr = nullptr;

// ---------------------------------------------------------------------------
// SpawnPromise<T>
// ---------------------------------------------------------------------------
template<typename T>
class SpawnPromise {
public:
    SpawnPromise() {
        if (tl_state_ptr<T>) state_ = *tl_state_ptr<T>;
    }

    // Eager start — body runs immediately
    std::suspend_never initial_suspend() noexcept { return {}; }

    // Final suspend — store done, resume waiter
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<SpawnPromise<T>> h) noexcept {
            auto& s = h.promise().state_;
            if (!s) return;
            s->done.store(true, std::memory_order_release);
            std::coroutine_handle<> w;
            { std::lock_guard lk(s->mutex); w = s->waiter; s->waiter = nullptr; }
            if (w && !w.done()) w.resume();
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_value(T v) { state_->result.template emplace<1>(std::move(v)); }
    void unhandled_exception() { state_->result.template emplace<2>(std::current_exception()); }

    JoinHandle<T> get_return_object();
    auto state() const noexcept { return state_; }

    // Clear waiter when the spawned coroutine frame is destroyed
    ~SpawnPromise() {
        if (state_) {
            std::lock_guard lk(state_->mutex);
            state_->waiter = nullptr;
        }
    }

private:
    std::shared_ptr<SpawnState<T>> state_;
};

// ---------------------------------------------------------------------------
// SpawnPromise<void>
// ---------------------------------------------------------------------------
template<>
class SpawnPromise<void> {
public:
    SpawnPromise() {
        if (tl_state_ptr<void>) state_ = *tl_state_ptr<void>;
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<SpawnPromise<void>> h) noexcept {
            auto& s = h.promise().state_;
            if (!s) return;
            s->done.store(true, std::memory_order_release);
            std::coroutine_handle<> w;
            { std::lock_guard lk(s->mutex); w = s->waiter; s->waiter = nullptr; }
            if (w && !w.done()) w.resume();
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() { state_->has_value = true; }
    void unhandled_exception() { state_->exception = std::current_exception(); }

    JoinHandle<void> get_return_object();
    auto state() const noexcept { return state_; }

    ~SpawnPromise() {
        if (state_) {
            std::lock_guard lk(state_->mutex);
            state_->waiter = nullptr;
        }
    }

private:
    std::shared_ptr<SpawnState<void>> state_;
};

// ---------------------------------------------------------------------------
// Eager coroutine wrappers
// ---------------------------------------------------------------------------
template<typename T>
JoinHandle<T> spawn_eager(Task<T> task) {
    co_return co_await std::move(task);
}

template<>
JoinHandle<void> spawn_eager(Task<void> task);

} // namespace detail

// ---------------------------------------------------------------------------
// JoinHandle<T>
//
// Returned by spawn().  co_await to get the result, or drop for fire-and-forget.
//
// Lifetime:
//   • If the task completes before the JoinHandle is destroyed, the coroutine
//     frame is cleaned up in the destructor (no leak).
//   • If the JoinHandle is moved into co_await, the frame is cleaned up when
//     the task completes (the promise releases the last shared_ptr ref).
//   • If the outer coroutine is destroyed while the task is still running,
//     the promise destructor clears the waiter handle for safety; the frame
//     is cleaned up when the task eventually completes.
// ---------------------------------------------------------------------------
template<typename T>
class JoinHandle {
public:
    using promise_type = detail::SpawnPromise<T>;

    JoinHandle() noexcept = default;

    explicit JoinHandle(std::coroutine_handle<promise_type> h,
                        std::shared_ptr<detail::SpawnState<T>> s) noexcept
        : handle_(h), state_(std::move(s)) {}

    JoinHandle(JoinHandle&& o) noexcept
        : handle_(std::exchange(o.handle_, nullptr))
        , state_(std::move(o.state_)) {}

    JoinHandle& operator=(JoinHandle&& o) noexcept {
        if (this != &o) { cleanup(); handle_ = std::exchange(o.handle_, nullptr); state_ = std::move(o.state_); }
        return *this;
    }

    JoinHandle(const JoinHandle&) = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    ~JoinHandle() { cleanup(); }

    // True once the spawned task has finished (lock-free)
    bool is_finished() const noexcept {
        return !state_ || state_->done.load(std::memory_order_acquire);
    }

    explicit operator bool() const noexcept { return state_ != nullptr; }

    // Explicitly detach — the spawned task continues running independently.
    // After detach(), the JoinHandle no longer owns the coroutine frame.
    void detach() noexcept { handle_ = nullptr; state_.reset(); }

    // Release the raw coroutine handle and detach this JoinHandle.
    // The caller takes ownership of the coroutine frame lifetime.
    // After release_handle(), this JoinHandle is empty.
    std::coroutine_handle<> release_handle() noexcept {
        auto h = std::exchange(handle_, nullptr);
        state_.reset();
        return h;
    }

    // co_await interface
    struct Awaiter {
        std::shared_ptr<detail::SpawnState<T>> state_;

        bool await_ready() noexcept {
            return state_->done.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> waiter) noexcept {
            std::lock_guard lk(state_->mutex);
            if (state_->done.load(std::memory_order_relaxed)) return false;
            state_->waiter = waiter;
            return true;
        }

        T await_resume() {
            if constexpr (std::is_void_v<T>) {
                if (state_->exception) std::rethrow_exception(state_->exception);
            } else {
                if (state_->result.index() == 2)
                    std::rethrow_exception(std::get<2>(state_->result));
                return std::move(std::get<1>(state_->result));
            }
        }
    };

    auto operator co_await() && noexcept {
        state_; // ensure state_ is valid
        return Awaiter{std::move(state_)};
    }

private:
    // Destroy the coroutine frame if the task is done and we still own the handle.
    // This prevents the frame leak for fire-and-forget and synchronous tasks.
    void cleanup() noexcept {
        if (handle_ && state_ && state_->done.load(std::memory_order_acquire)) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    std::coroutine_handle<promise_type> handle_ = nullptr;
    std::shared_ptr<detail::SpawnState<T>> state_;
};

// ---------------------------------------------------------------------------
// spawn() — eagerly start a Task<T>, return a JoinHandle<T>
// ---------------------------------------------------------------------------
template<typename T>
JoinHandle<T> spawn(Task<T> task) {
    auto state = std::make_shared<detail::SpawnState<T>>();
    detail::tl_state_ptr<T> = &state;
    auto jh = detail::spawn_eager<T>(std::move(task));
    detail::tl_state_ptr<T> = nullptr;
    return jh;
}

// ---------------------------------------------------------------------------
// Out-of-line definitions
// ---------------------------------------------------------------------------
namespace detail {

template<typename T>
JoinHandle<T> SpawnPromise<T>::get_return_object() {
    return JoinHandle<T>{
        std::coroutine_handle<SpawnPromise<T>>::from_promise(*this), state_};
}

inline JoinHandle<void> SpawnPromise<void>::get_return_object() {
    return JoinHandle<void>{
        std::coroutine_handle<SpawnPromise<void>>::from_promise(*this), state_};
}

template<>
inline JoinHandle<void> spawn_eager(Task<void> task) {
    co_await std::move(task);
}

} // namespace detail

} // namespace async_net
