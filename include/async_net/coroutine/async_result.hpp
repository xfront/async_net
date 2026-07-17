#pragma once

#include <coroutine>
#include <optional>
#include <exception>
#include <variant>
#include <utility>

namespace async_net {

// AsyncResult bridges callback-based I/O operations with coroutines.
// The I/O backend stores a pointer to this and calls complete() when done.
// The coroutine co_awaits this to suspend until completion.
template<typename T = void>
class AsyncResult {
public:
    AsyncResult() = default;

    AsyncResult(const AsyncResult&) = delete;
    AsyncResult& operator=(const AsyncResult&) = delete;

    // Called by I/O backend when operation completes
    void complete(T value) {
        result_.template emplace<1>(std::move(value));
        if (handle_) {
            handle_.resume();
        }
    }

    // Called by I/O backend when operation fails
    void complete_error(std::exception_ptr ex) {
        result_.template emplace<2>(std::move(ex));
        if (handle_) {
            handle_.resume();
        }
    }

    // Awaiter interface
    bool await_ready() const noexcept {
        return result_.index() != 0;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        handle_ = h;
        // Check if already completed between construction and suspend
        return result_.index() == 0;
    }

    T await_resume() {
        if (result_.index() == 2) {
            std::rethrow_exception(std::get<2>(result_));
        }
        return std::get<1>(result_);
    }

private:
    std::variant<std::monostate, T, std::exception_ptr> result_;
    std::coroutine_handle<> handle_;
};

// Specialization for void
template<>
class AsyncResult<void> {
public:
    AsyncResult() = default;

    AsyncResult(const AsyncResult&) = delete;
    AsyncResult& operator=(const AsyncResult&) = delete;

    void complete() {
        completed_ = true;
        if (handle_) {
            handle_.resume();
        }
    }

    void complete_error(std::exception_ptr ex) {
        exception_ = std::move(ex);
        completed_ = true;
        if (handle_) {
            handle_.resume();
        }
    }

    bool await_ready() const noexcept {
        return completed_;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        handle_ = h;
        return !completed_;
    }

    void await_resume() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    bool completed_ = false;
    std::exception_ptr exception_;
    std::coroutine_handle<> handle_;
};

// Specialization for size_t (common for read/write operations)
// Uses the generic template above

} // namespace async_net
