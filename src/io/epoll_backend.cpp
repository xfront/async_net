#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_LINUX

#include "epoll_backend.hpp"
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace async_net {

EpollBackend::EpollBackend() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll fd");
    }
}

EpollBackend::~EpollBackend() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool EpollBackend::register_socket(socket_t fd) {
    if (set_nonblocking(fd) != 0) {
        return false;
    }

    struct epoll_event ev{};
    ev.events = 0;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return false;
    }

    fd_events_[fd] = 0;
    return true;
}

void EpollBackend::deregister_socket(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    read_ops_.erase(fd);
    write_ops_.erase(fd);
    fd_events_.erase(fd);
}

void EpollBackend::async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::Read, buf, len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLIN);

    // Try to complete immediately (non-blocking)
    try_complete_read(fd);
}

void EpollBackend::async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    write_ops_[fd] = PendingOp{OpType::Write, const_cast<void*>(buf), len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLOUT);

    // Try to complete immediately
    try_complete_write(fd);
}

void EpollBackend::async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[listen_fd] = PendingOp{OpType::Accept, nullptr, 0, out_fd, ctx, {}, 0};

    update_events(listen_fd, EPOLLIN);

    // Try to complete immediately
    auto it = read_ops_.find(listen_fd);
    if (it != read_ops_.end()) {
        socket_t new_fd = ::accept(listen_fd, nullptr, nullptr);
        if (new_fd != invalid_socket) {
            set_nonblocking(new_fd);
            *it->second.out_fd = new_fd;
            it->second.ctx->complete(0, 0);
            read_ops_.erase(it);
            update_events(listen_fd, 0);
        }
    }
}

void EpollBackend::async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    int ret = ::connect(fd, addr, addrlen);
    if (ret == 0) {
        ctx->complete(0, 0);
        return;
    }

    if (errno != EINPROGRESS) {
        ctx->complete(-1, errno);
        return;
    }

    // Connection in progress, wait for writability
    write_ops_[fd] = PendingOp{OpType::Connect, nullptr, 0, nullptr, ctx, {}, 0};
    update_events(fd, EPOLLOUT);
}

void EpollBackend::async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::RecvFrom, buf, len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLIN);

    // Try to complete immediately (non-blocking)
    try_complete_read(fd);
}

void EpollBackend::async_sendto(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    PendingOp op{OpType::SendTo, const_cast<void*>(buf), len, nullptr, ctx, {}, 0};
    if (to && tolen > 0 && tolen <= sizeof(op.dest_addr)) {
        memcpy(&op.dest_addr, to, tolen);
        op.dest_len = tolen;
    }
    write_ops_[fd] = std::move(op);

    update_events(fd, EPOLLOUT);

    // Try to complete immediately
    try_complete_write(fd);
}

void EpollBackend::poll(int timeout_ms) {
    int n = epoll_wait(epoll_fd_, events_, MAX_EVENTS, timeout_ms);

    // Collect completed contexts to resume AFTER processing all events
    std::vector<OperationContext*> to_resume;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (int i = 0; i < n; ++i) {
            socket_t fd = events_[i].data.fd;
            uint32_t revents = events_[i].events;

            // Handle read events
            if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                auto it = read_ops_.find(fd);
                if (it != read_ops_.end()) {
                    if (it->second.type == OpType::Read) {
                        ssize_t bytes = ::recv(fd, it->second.buf, it->second.len, 0);
                        if (bytes >= 0) {
                            it->second.ctx->complete(bytes, 0);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        }
                    } else if (it->second.type == OpType::RecvFrom) {
                        struct sockaddr_in from_addr{};
                        socklen_t from_len = sizeof(from_addr);
                        ssize_t bytes = ::recvfrom(fd, it->second.buf, it->second.len, 0,
                                                   reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
                        if (bytes >= 0) {
                            memcpy(&it->second.ctx->from_addr_, &from_addr, from_len);
                            it->second.ctx->from_len_ = from_len;
                            it->second.ctx->complete(bytes, 0);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        }
                    } else if (it->second.type == OpType::Accept) {
                        socket_t new_fd = ::accept(fd, nullptr, nullptr);
                        if (new_fd != invalid_socket) {
                            set_nonblocking(new_fd);
                            *it->second.out_fd = new_fd;
                            it->second.ctx->complete(0, 0);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx.get());
                            read_ops_.erase(it);
                        }
                    }
                }
            }

            // Handle write events
            if (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
                auto it = write_ops_.find(fd);
                if (it != write_ops_.end()) {
                    if (it->second.type == OpType::Write || it->second.type == OpType::SendTo) {
                        ssize_t bytes;
                        if (it->second.type == OpType::SendTo && it->second.dest_len > 0) {
                            bytes = ::sendto(fd, it->second.buf, it->second.len, MSG_NOSIGNAL,
                                             reinterpret_cast<const struct sockaddr*>(&it->second.dest_addr),
                                             it->second.dest_len);
                        } else {
                            bytes = ::send(fd, it->second.buf, it->second.len, MSG_NOSIGNAL);
                        }
                        if (bytes >= 0) {
                            it->second.ctx->complete(bytes, 0);
                            to_resume.push_back(it->second.ctx.get());
                            write_ops_.erase(it);
                            update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx.get());
                            write_ops_.erase(it);
                            update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                        }
                    } else if (it->second.type == OpType::Connect) {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        it->second.ctx->complete(err == 0 ? 0 : -1, err);
                        to_resume.push_back(it->second.ctx.get());
                        write_ops_.erase(it);
                        update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                    }
                }
            }
        }
    }

    // Resume all completed coroutines outside the lock
    for (auto* ctx : to_resume) {
        ctx->resume();
    }
}

void EpollBackend::update_events(socket_t fd, uint32_t events) {
    // Must be called with mutex_ held
    uint32_t new_events = 0;

    if (read_ops_.count(fd) && write_ops_.count(fd)) {
        new_events = EPOLLIN | EPOLLOUT;
    } else if (read_ops_.count(fd)) {
        new_events = EPOLLIN;
    } else if (write_ops_.count(fd)) {
        new_events = EPOLLOUT;
    }

    auto it = fd_events_.find(fd);
    if (it != fd_events_.end() && it->second == new_events) {
        return;
    }

    struct epoll_event ev{};
    ev.events = new_events;
    ev.data.fd = fd;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    if (ret < 0) {
        // If MOD fails (fd not in epoll), try ADD
        if (errno == ENOENT) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        }
    }
    fd_events_[fd] = new_events;
}

void EpollBackend::try_complete_read(socket_t fd) {
    // Must be called with mutex_ held
    auto it = read_ops_.find(fd);
    if (it == read_ops_.end()) return;

    if (it->second.type == OpType::RecvFrom) {
        struct sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = ::recvfrom(fd, it->second.buf, it->second.len, 0,
                               reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
        if (n >= 0) {
            memcpy(&it->second.ctx->from_addr_, &from_addr, from_len);
            it->second.ctx->from_len_ = from_len;
            it->second.ctx->complete(n, 0);
            read_ops_.erase(it);
            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
        } else if (!is_would_block()) {
            it->second.ctx->complete(-1, errno);
            read_ops_.erase(it);
            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
        }
    } else {
        ssize_t n = ::recv(fd, it->second.buf, it->second.len, 0);
        if (n >= 0) {
            it->second.ctx->complete(n, 0);
            read_ops_.erase(it);
            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
        } else if (!is_would_block()) {
            it->second.ctx->complete(-1, errno);
            read_ops_.erase(it);
            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
        }
    }
    // If would_block, just wait for epoll event
}

void EpollBackend::try_complete_write(socket_t fd) {
    // Must be called with mutex_ held
    auto it = write_ops_.find(fd);
    if (it == write_ops_.end()) return;

    ssize_t n;
    if (it->second.type == OpType::SendTo && it->second.dest_len > 0) {
        n = ::sendto(fd, it->second.buf, it->second.len, MSG_NOSIGNAL,
                     reinterpret_cast<const struct sockaddr*>(&it->second.dest_addr),
                     it->second.dest_len);
    } else {
        n = ::send(fd, it->second.buf, it->second.len, MSG_NOSIGNAL);
    }
    if (n >= 0) {
        it->second.ctx->complete(n, 0);
        write_ops_.erase(it);
        update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
    } else if (!is_would_block()) {
        it->second.ctx->complete(-1, errno);
        write_ops_.erase(it);
        update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
    }
    // If would_block, just wait for epoll event
}

} // namespace async_net

#endif // ASYNC_NET_LINUX
