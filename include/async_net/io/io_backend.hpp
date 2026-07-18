#pragma once

#include "../detail/config.hpp"
#include <cstddef>
#include <memory>

namespace async_net {

class OperationContext;

// Abstract I/O backend interface
// Each platform (epoll, kqueue, iocp, io_uring) implements this interface.
// The backend takes shared ownership of the OperationContext via shared_ptr
// to ensure the context survives even after the awaiter is destroyed.
class IoBackend {
public:
    virtual ~IoBackend() = default;

    // Poll for I/O events. timeout_ms < 0 means block indefinitely.
    virtual void poll(int timeout_ms) = 0;

    // Register a socket with the backend
    virtual bool register_socket(socket_t fd) = 0;

    // Deregister a socket from the backend
    virtual void deregister_socket(socket_t fd) = 0;

    // Submit an async read operation. Backend takes shared ownership of ctx.
    virtual void async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) = 0;

    // Submit an async write operation
    virtual void async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) = 0;

    // Submit an async accept operation
    virtual void async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) = 0;

    // Submit an async connect operation
    virtual void async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) = 0;

    // Submit an async recvfrom operation (captures sender address in ctx->from_addr_)
    virtual void async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_read(fd, buf, len, std::move(ctx)); // default: same as read
    }

    // Submit an async sendto operation
    virtual void async_sendto(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) {
        async_write(fd, buf, len, std::move(ctx)); // default: same as write
    }

    // Wait for socket readability (for SSL WANT_READ integration)
    virtual void async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) = 0;

    // Wait for socket writability (for SSL WANT_WRITE integration)
    virtual void async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) = 0;

    // Convenience wrappers
    virtual void async_read_some(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_read(fd, buf, len, std::move(ctx));
    }

    virtual void async_write_some(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_write(fd, buf, len, std::move(ctx));
    }

    // Wake up the backend from poll() (called when work is posted from another thread).
    // Default: no-op. Backends should override to provide efficient wakeup.
    virtual void wake() {}

    // Get the backend name
    virtual const char* name() const = 0;
};

// Create the default backend for the current platform
std::unique_ptr<IoBackend> create_default_backend();

} // namespace async_net
