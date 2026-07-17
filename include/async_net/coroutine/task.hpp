#pragma once

#include <coroutine>
#include <exception>
#include <variant>
#include <optional>
#include <utility>
#include <cassert>

namespace async_net {

// Forward declaration
template<typename T = void>
class Task;

namespace detail {

class TaskPromiseBase {
public:
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
            auto& promise = h.promise();
            if (promise.continuation_) {
                return promise.continuation_;
            }
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void set_continuation(std::coroutine_handle<> cont) noexcept {
        continuation_ = cont;
    }

protected:
    std::coroutine_handle<> continuation_;
};

template<typename T>
class TaskPromise : public TaskPromiseBase {
public:
    Task<T> get_return_object() noexcept;

    void return_value(T value) {
        result_.template emplace<1>(std::move(value));
    }

    void unhandled_exception() {
        result_.template emplace<2>(std::current_exception());
    }

    T result() {
        if (result_.index() == 2) {
            std::rethrow_exception(std::get<2>(result_));
        }
        return std::get<1>(result_);
    }

private:
    std::variant<std::monostate, T, std::exception_ptr> result_;
};

template<>
class TaskPromise<void> : public TaskPromiseBase {
public:
    Task<void> get_return_object() noexcept;

    void return_void() {}

    void unhandled_exception() {
        exception_ = std::current_exception();
    }

    void result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    std::exception_ptr exception_;
};

} // namespace detail

// Task<T> - lazy coroutine that starts only when co_awaited
template<typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using value_type = T;

    Task() noexcept : handle_(nullptr) {}

    explicit Task(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    // Check if task is valid
    explicit operator bool() const noexcept {
        return handle_ && !handle_.done();
    }

    // Check if task is done
    bool done() const noexcept {
        return !handle_ || handle_.done();
    }

    // Resume the coroutine
    void resume() {
        assert(handle_ && !handle_.done());
        handle_.resume();
    }

    // Get the coroutine handle
    std::coroutine_handle<promise_type> handle() const noexcept {
        return handle_;
    }

    // Release ownership of the handle
    std::coroutine_handle<promise_type> release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    // Awaiter interface - allows co_await on Task
    struct Awaiter {
        std::coroutine_handle<promise_type> handle_;

        ~Awaiter() {
            if (handle_) handle_.destroy();
        }

        bool await_ready() noexcept {
            return !handle_ || handle_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
            handle_.promise().set_continuation(awaiting);
            return handle_;
        }

        T await_resume() {
            if constexpr (std::is_void_v<T>) {
                handle_.promise().result();
            } else {
                return handle_.promise().result();
            }
        }
    };

    auto operator co_await() && noexcept {
        return Awaiter{std::exchange(handle_, nullptr)};
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

} // namespace detail

} // namespace async_net
