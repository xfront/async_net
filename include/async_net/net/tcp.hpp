#pragma once

#include "socket.hpp"
#include "../detail/config.hpp"
#include <memory>
#include <cstring>

namespace async_net {

namespace tcp {

// TCP socket
class socket : public socket_base {
public:
    socket(io_context& ctx)
        : socket_base(ctx, ::socket(AF_INET, SOCK_STREAM, 0)) {
        if (fd_ != invalid_socket) {
            ctx_->backend().register_socket(fd_);
        }
    }

    socket(io_context& ctx, socket_t fd)
        : socket_base(ctx, fd) {
        if (fd_ != invalid_socket) {
            ctx_->backend().register_socket(fd_);
        }
    }

    socket(socket&& other) noexcept = default;
    socket& operator=(socket&& other) noexcept = default;

    // Async read some data
    auto async_read_some(mutable_buffer buf) {
        struct ReadAwaiter {
            socket& sock_;
            mutable_buffer buf_;
            std::shared_ptr<ReadContext> ctx_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<ReadContext>();
                ctx_->set_handle(h);
                sock_.get_io_context().backend().async_read_some(
                    sock_.native_handle(), buf_.data(), buf_.size(), ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            ssize_t await_resume() const {
                return ctx_->result();
            }
        };
        return ReadAwaiter{*this, buf, nullptr};
    }

    // Async write some data
    auto async_write_some(const_buffer buf) {
        struct WriteAwaiter {
            socket& sock_;
            const_buffer buf_;
            std::shared_ptr<WriteContext> ctx_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<WriteContext>();
                ctx_->set_handle(h);
                sock_.get_io_context().backend().async_write_some(
                    sock_.native_handle(), buf_.data(), buf_.size(), ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            ssize_t await_resume() const {
                return ctx_->result();
            }
        };
        return WriteAwaiter{*this, buf, nullptr};
    }

    // Async write all data (ensures all bytes are written)
    Task<size_t> async_write(const_buffer buf) {
        size_t total = 0;
        const char* data = static_cast<const char*>(buf.data());
        size_t remaining = buf.size();

        while (remaining > 0) {
            auto n = co_await async_write_some(const_buffer(data + total, remaining));
            if (n <= 0) break;
            total += n;
            remaining -= n;
        }

        co_return total;
    }

    // Async connect to endpoint
    auto async_connect(const struct sockaddr* addr, socklen_t addrlen) {
        struct ConnectAwaiter {
            socket& sock_;
            const struct sockaddr* addr_;
            socklen_t addrlen_;
            std::shared_ptr<ConnectContext> ctx_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<ConnectContext>();
                ctx_->set_handle(h);
                sock_.get_io_context().backend().async_connect(
                    sock_.native_handle(), addr_, addrlen_, ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            ssize_t await_resume() const {
                return ctx_->result();
            }
        };
        return ConnectAwaiter{*this, addr, addrlen, nullptr};
    }

    // Convenience: connect to host:port
    Task<int> async_connect(const char* host, uint16_t port) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            co_return -1;
        }

        co_return co_await async_connect(
            reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    }
};

// TCP acceptor
class acceptor {
public:
    acceptor(io_context& ctx, uint16_t port, const char* addr = "0.0.0.0")
        : ctx_(&ctx) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == invalid_socket) {
            return;
        }

        set_reuse_addr(fd_);
        set_nonblocking(fd_);
        ctx_->backend().register_socket(fd_);

        struct sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        ::inet_pton(AF_INET, addr, &saddr.sin_addr);

        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
            close();
            return;
        }

        if (::listen(fd_, SOMAXCONN) < 0) {
            close();
            return;
        }
    }

    acceptor(acceptor&& other) noexcept
        : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket)) {}

    acceptor& operator=(acceptor&& other) noexcept {
        if (this != &other) {
            close();
            ctx_ = other.ctx_;
            fd_ = std::exchange(other.fd_, invalid_socket);
        }
        return *this;
    }

    ~acceptor() {
        close();
    }

    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    void close() {
        if (fd_ != invalid_socket) {
            ctx_->backend().deregister_socket(fd_);
            close_socket(fd_);
            fd_ = invalid_socket;
        }
    }

    bool is_open() const { return fd_ != invalid_socket; }

    // Async accept a connection
    auto async_accept() {
        struct AcceptAwaiter {
            acceptor& acc_;
            std::shared_ptr<AcceptContext> ctx_;
            socket_t new_fd_;

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) {
                ctx_ = std::make_shared<AcceptContext>();
                ctx_->set_handle(h);
                new_fd_ = invalid_socket;
                acc_.ctx_->backend().async_accept(
                    acc_.fd_, &new_fd_, ctx_);
                if (ctx_->completed()) {
                    return false;
                }
                return true;
            }

            socket await_resume() {
                if (ctx_->error() == 0 && new_fd_ != invalid_socket) {
                    return socket(*acc_.ctx_, new_fd_);
                }
                return socket(*acc_.ctx_, invalid_socket);
            }
        };

        return AcceptAwaiter{*this, nullptr, invalid_socket};
    }

    io_context& get_io_context() { return *ctx_; }

private:
    io_context* ctx_;
    socket_t fd_;
};

} // namespace tcp

} // namespace async_net
