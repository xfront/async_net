#pragma once

#include "socket.hpp"
#include "../detail/config.hpp"
#include <memory>
#include <cstring>

namespace async_net {

namespace udp {

// UDP endpoint
class endpoint {
public:
    endpoint() {
        memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_addr.s_addr = INADDR_ANY;
        addr_.sin_port = 0;
    }

    endpoint(uint16_t port, const char* addr = "0.0.0.0") {
        memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        ::inet_pton(AF_INET, addr, &addr_.sin_addr);
    }

    endpoint(const struct sockaddr_in& addr) : addr_(addr) {}

    const struct sockaddr_in& data() const { return addr_; }
    struct sockaddr_in& data() { return addr_; }

    const struct sockaddr* sockaddr_ptr() const {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }

    socklen_t size() const { return sizeof(addr_); }

    uint16_t port() const { return ntohs(addr_.sin_port); }

    std::string address() const {
        char buf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }

private:
    struct sockaddr_in addr_;
};

// UDP socket
class socket : public socket_base {
public:
    socket(io_context& ctx)
        : socket_base(ctx, ::socket(AF_INET, SOCK_DGRAM, 0)) {
        if (fd_ != invalid_socket) {
            ctx_->backend().register_socket(fd_);
        }
    }

    socket(io_context& ctx, socket_t fd)
        : socket_base(ctx, fd) {}

    socket(socket&& other) noexcept = default;
    socket& operator=(socket&& other) noexcept = default;

    // Bind to local endpoint
    bool bind(const endpoint& ep) {
        return ::bind(fd_, ep.sockaddr_ptr(), ep.size()) == 0;
    }

    // Async receive data
    auto async_receive_from(mutable_buffer buf, endpoint& from) {
        struct RecvFromAwaiter {
            socket& sock_;
            mutable_buffer buf_;
            endpoint& from_;
            std::shared_ptr<ReadContext> ctx_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<ReadContext>();
                ctx_->set_handle(h);
                sock_.get_io_context().backend().async_recvfrom(
                    sock_.native_handle(), buf_.data(), buf_.size(), ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            ssize_t await_resume() {
                // Copy sender address from context
                if (ctx_->from_len_ > 0) {
                    auto* sin = reinterpret_cast<struct sockaddr_in*>(&ctx_->from_addr_);
                    from_ = endpoint(*sin);
                }
                return ctx_->result();
            }
        };
        return RecvFromAwaiter{*this, buf, from, nullptr};
    }

    // Async send data to endpoint
    auto async_send_to(const_buffer buf, const endpoint& to) {
        struct SendToAwaiter {
            socket& sock_;
            const_buffer buf_;
            endpoint to_;
            std::shared_ptr<WriteContext> ctx_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<WriteContext>();
                ctx_->set_handle(h);
                sock_.get_io_context().backend().async_sendto(
                    sock_.native_handle(), buf_.data(), buf_.size(),
                    to_.sockaddr_ptr(), to_.size(), ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            ssize_t await_resume() {
                return ctx_->result();
            }
        };
        return SendToAwaiter{*this, buf, to, nullptr};
    }

    // Synchronous receive (for simple cases)
    ssize_t receive_from(mutable_buffer buf, endpoint& from) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
#ifdef ASYNC_NET_WINDOWS
        ssize_t n = ::recvfrom(fd_, static_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                               reinterpret_cast<struct sockaddr*>(&from_addr),
                               reinterpret_cast<int*>(&from_len));
#else
        ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                               reinterpret_cast<struct sockaddr*>(&from_addr),
                               &from_len);
#endif
        if (n >= 0) {
            from = endpoint(from_addr);
        }
        return n;
    }

    // Synchronous send (for simple cases)
    ssize_t send_to(const_buffer buf, const endpoint& to) {
#ifdef ASYNC_NET_WINDOWS
        return ::sendto(fd_, static_cast<const char*>(buf.data()), static_cast<int>(buf.size()), 0,
                       to.sockaddr_ptr(), static_cast<int>(to.size()));
#else
        return ::sendto(fd_, buf.data(), buf.size(), 0,
                       to.sockaddr_ptr(), to.size());
#endif
    }
};

} // namespace udp

} // namespace async_net
