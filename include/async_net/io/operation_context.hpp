#pragma once

#include "../detail/config.hpp"
#include <coroutine>
#include <cstddef>
#include <atomic>
#ifndef ASYNC_NET_WINDOWS
#include <sys/socket.h>
#endif

namespace async_net {

// Operation type enumeration
enum class OpType {
    Read,
    Write,
    Accept,
    Connect,
    RecvFrom,
    SendTo,
    Timer,
    WaitReadable,   // Wait for socket readability (for SSL integration)
    WaitWritable,   // Wait for socket writability (for SSL integration)
    Custom
};

// OperationContext holds the state for an async operation.
// The awaiter sets the coroutine handle, then submits the op to the backend.
// When the backend calls complete(), it stores the result and sets the
// completed flag. The awaiter checks if the operation completed synchronously
// (before await_suspend returned) and handles resumption accordingly.
class OperationContext {
public:
    OperationContext() = default;
    ~OperationContext() = default;

    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;

    // Set the coroutine handle (called by the awaiter before submitting)
    void set_handle(std::coroutine_handle<> h) noexcept {
        handle_ = h;
    }

    std::coroutine_handle<> handle() const noexcept {
        return handle_;
    }

    // Called by the backend when the operation completes.
    // Stores the result and marks as completed.
    // Does NOT resume the coroutine - the awaiter/event loop handles that.
    void complete(ssize_t result, int error_code) {
        result_ = result;
        error_code_ = error_code;
        completed_.store(true, std::memory_order_release);
    }

    // Returns the result (bytes transferred, or -1 on error)
    ssize_t result() const noexcept {
        if (error_code_ != 0) {
            return -1;
        }
        return result_;
    }

    // Get error code
    int error() const noexcept { return error_code_; }

    // Get operation type
    OpType type() const noexcept { return type_; }

    void set_type(OpType t) { type_ = t; }

    // Check if the backend has completed this operation
    bool completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }

    // Resume the coroutine (called by the event loop or awaiter)
    void resume() {
        if (handle_) {
            handle_.resume();
        }
    }

    // Sender address (populated by recvfrom operations)
    struct sockaddr_storage from_addr_{};
    socklen_t from_len_ = 0;

protected:
    OpType type_ = OpType::Read;
    ssize_t result_ = 0;
    int error_code_ = 0;
    std::atomic<bool> completed_{false};
    std::coroutine_handle<> handle_;
};

// Specialized operation contexts for different operation types
class ReadContext : public OperationContext {
public:
    ReadContext() { set_type(OpType::Read); }
};

class WriteContext : public OperationContext {
public:
    WriteContext() { set_type(OpType::Write); }
};

class AcceptContext : public OperationContext {
public:
    AcceptContext() { set_type(OpType::Accept); }
    socket_t* accepted_fd = nullptr;
};

class ConnectContext : public OperationContext {
public:
    ConnectContext() { set_type(OpType::Connect); }
};

} // namespace async_net
