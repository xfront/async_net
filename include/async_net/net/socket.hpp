#pragma once

#include "../detail/config.hpp"
#include "../io/io_context.hpp"
#include "../io/operation_context.hpp"
#include "buffer.hpp"
#include <utility>
#include <memory>

namespace async_net {

// Base socket class with common functionality
class socket_base {
public:
    socket_base(io_context& ctx, socket_t fd)
        : ctx_(&ctx), fd_(fd) {}

    socket_base(socket_base&& other) noexcept
        : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket)) {}

    socket_base& operator=(socket_base&& other) noexcept {
        if (this != &other) {
            close();
            ctx_ = other.ctx_;
            fd_ = std::exchange(other.fd_, invalid_socket);
        }
        return *this;
    }

    socket_base(const socket_base&) = delete;
    socket_base& operator=(const socket_base&) = delete;

    ~socket_base() {
        close();
    }

    // Close the socket
    void close() {
        if (fd_ != invalid_socket) {
            ctx_->backend().deregister_socket(fd_);
            close_socket(fd_);
            fd_ = invalid_socket;
        }
    }

    // Check if socket is open
    bool is_open() const { return fd_ != invalid_socket; }

    // Get the native socket handle
    socket_t native_handle() const { return fd_; }

    // Get the io_context
    io_context& get_io_context() { return *ctx_; }

    // Set TCP_NODELAY
    bool set_no_delay(bool enable) {
        int opt = enable ? 1 : 0;
        return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
    }

    // Get local endpoint
    struct sockaddr_storage local_address() const {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
        return addr;
    }

    // Get remote endpoint
    struct sockaddr_storage remote_address() const {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        ::getpeername(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
        return addr;
    }

    // ---- Broadcast options ----

    // Enable/disable SO_BROADCAST
    bool set_broadcast(bool enable) {
        int opt = enable ? 1 : 0;
        return ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
    }

    // ---- Multicast options ----

    // Set multicast TTL (0-255). 0 = local subnet only.
    bool set_multicast_ttl(int ttl) {
        unsigned char t = static_cast<unsigned char>(ttl);
        return ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                           reinterpret_cast<const char*>(&t), sizeof(t)) == 0;
    }

    // Enable/disable multicast loopback
    bool set_multicast_loopback(bool enable) {
        unsigned char opt = enable ? 1 : 0;
        return ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
    }

    // Set outgoing interface for multicast (by local address)
    bool set_multicast_interface(const char* local_addr) {
        struct in_addr addr{};
        if (local_addr) {
            if (::inet_pton(AF_INET, local_addr, &addr) <= 0) return false;
        } else {
            addr.s_addr = htonl(INADDR_ANY); // let kernel choose
        }
        return ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF,
                           reinterpret_cast<const char*>(&addr), sizeof(addr)) == 0;
    }

    // Join a multicast group. group_addr e.g. "239.0.0.1",
    // interface_addr e.g. "0.0.0.0" for any interface.
    bool join_multicast_group(const char* group_addr, const char* interface_addr = "0.0.0.0") {
        struct ip_mreq mreq{};
        if (::inet_pton(AF_INET, group_addr, &mreq.imr_multiaddr) <= 0) return false;
        if (::inet_pton(AF_INET, interface_addr, &mreq.imr_interface) <= 0) return false;
        return ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
    }

    // Leave a multicast group
    bool leave_multicast_group(const char* group_addr, const char* interface_addr = "0.0.0.0") {
        struct ip_mreq mreq{};
        if (::inet_pton(AF_INET, group_addr, &mreq.imr_multiaddr) <= 0) return false;
        if (::inet_pton(AF_INET, interface_addr, &mreq.imr_interface) <= 0) return false;
        return ::setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                           reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
    }

    // Allow multiple sockets to bind to the same address/port (useful for multicast)
    bool set_reuse_address(bool enable) {
        int opt = enable ? 1 : 0;
        return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
    }

    // Allow multiple sockets to bind to the same port (macOS/BSD need SO_REUSEPORT)
    bool set_reuse_port(bool enable) {
#ifdef SO_REUSEPORT
        int opt = enable ? 1 : 0;
        return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
#else
        (void)enable;
        return false;
#endif
    }

protected:
    io_context* ctx_;
    socket_t fd_;
};

} // namespace async_net
