#pragma once

#include "../detail/config.hpp"
#include <cstddef>
#include <memory>

namespace async_net {

class OperationContext;

// ============================================================================
// Abstract I/O backend interface (virtual dispatch — for type erasure)
// ============================================================================

class IoBackend {
public:
    virtual ~IoBackend() = default;

    virtual void poll(int timeout_ms) = 0;
    virtual bool register_socket(socket_t fd) = 0;
    virtual void deregister_socket(socket_t fd) = 0;
    virtual void async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) = 0;
    virtual void async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) = 0;
    virtual void async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) = 0;
    virtual void async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) = 0;

    virtual void async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_read(fd, buf, len, std::move(ctx));
    }

    virtual void async_sendto(socket_t fd, const void* buf, size_t len,
                              const struct sockaddr* to, socklen_t tolen,
                              std::shared_ptr<OperationContext> ctx) {
        async_write(fd, buf, len, std::move(ctx));
    }

    virtual void async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) = 0;
    virtual void async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) = 0;

    virtual void async_read_some(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_read(fd, buf, len, std::move(ctx));
    }

    virtual void async_write_some(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
        async_write(fd, buf, len, std::move(ctx));
    }

    virtual void wake() {}
    virtual bool supports_concurrent_poll() const { return false; }
    virtual const char* name() const = 0;
};

// ============================================================================
// CRTP base — compile-time dispatch, eliminates boilerplate in backends
// ============================================================================
// Concrete backends inherit this and provide _impl methods.
// Example:
//   class EpollBackend : public IoBackendBase<EpollBackend> {
//       void poll_impl(int timeout_ms);
//       static constexpr bool concurrent_poll = false;
//       static constexpr const char* backend_name = "epoll";
//   };

template<typename Derived>
class IoBackendBase : public IoBackend {
public:
    void poll(int timeout_ms) final { derived().poll_impl(timeout_ms); }
    bool register_socket(socket_t fd) final { return derived().register_impl(fd); }
    void deregister_socket(socket_t fd) final { derived().deregister_impl(fd); }

    void async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) final {
        derived().async_read_impl(fd, buf, len, std::move(ctx));
    }

    void async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) final {
        derived().async_write_impl(fd, buf, len, std::move(ctx));
    }

    void async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) final {
        derived().async_accept_impl(listen_fd, out_fd, std::move(ctx));
    }

    void async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen,
                       std::shared_ptr<OperationContext> ctx) final {
        derived().async_connect_impl(fd, addr, addrlen, std::move(ctx));
    }

    void async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) final {
        if constexpr (requires { derived().async_recvfrom_impl(fd, buf, len, std::shared_ptr<OperationContext>{}); }) {
            derived().async_recvfrom_impl(fd, buf, len, std::move(ctx));
        } else {
            derived().async_read_impl(fd, buf, len, std::move(ctx));
        }
    }

    void async_sendto(socket_t fd, const void* buf, size_t len,
                      const struct sockaddr* to, socklen_t tolen,
                      std::shared_ptr<OperationContext> ctx) final {
        if constexpr (requires { derived().async_sendto_impl(fd, buf, len, to, tolen, std::shared_ptr<OperationContext>{}); }) {
            derived().async_sendto_impl(fd, buf, len, to, tolen, std::move(ctx));
        } else {
            derived().async_write_impl(fd, buf, len, std::move(ctx));
        }
    }

    void async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) final {
        derived().async_wait_readable_impl(fd, std::move(ctx));
    }

    void async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) final {
        derived().async_wait_writable_impl(fd, std::move(ctx));
    }

    void wake() final {
        if constexpr (requires { derived().wake_impl(); }) {
            derived().wake_impl();
        }
    }

    bool supports_concurrent_poll() const final {
        return Derived::concurrent_poll;
    }

    const char* name() const final {
        return Derived::backend_name;
    }

private:
    Derived& derived() noexcept { return static_cast<Derived&>(*this); }
    const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }
};

// ============================================================================
// Default backend type for compile-time backend selection
// ============================================================================

#ifdef ASYNC_NET_WINDOWS
class IocpBackend;
using default_backend_t = IocpBackend;
#elif defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)
class KqueueBackend;
using default_backend_t = KqueueBackend;
#else
// Linux: runtime detection between io_uring and epoll
// Use EpollBackend as compile-time default (most compatible)
class EpollBackend;
using default_backend_t = EpollBackend;
#endif

// Factory function for runtime backend selection
std::unique_ptr<IoBackend> create_default_backend();

} // namespace async_net
