#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_LINUX

#include "epoll_backend.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace async_net {

// Thread-local deferred waits for handling coroutines that resume synchronously
// inside poll() while the mutex is held.
thread_local std::vector<EpollBackend::DeferredWait> EpollBackend::tl_deferred_waits_;
thread_local bool EpollBackend::tl_in_poll_ = false;

EpollBackend::EpollBackend() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll fd");
    }

    // Create eventfd for cross-thread wakeup
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ >= 0) {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }
}

EpollBackend::~EpollBackend() {
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool EpollBackend::register_impl(socket_t fd) {
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

void EpollBackend::deregister_impl(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    read_ops_.erase(fd);
    write_ops_.erase(fd);
    fd_events_.erase(fd);
}

void EpollBackend::async_read_impl(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::Read, buf, len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLIN);

    // Try to complete immediately (non-blocking)
    try_complete_read(fd);
}

void EpollBackend::async_write_impl(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    write_ops_[fd] = PendingOp{OpType::Write, const_cast<void*>(buf), len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLOUT);

    // Try to complete immediately
    try_complete_write(fd);
}

void EpollBackend::async_accept_impl(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) {
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

void EpollBackend::async_connect_impl(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) {
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

void EpollBackend::async_recvfrom_impl(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::RecvFrom, buf, len, nullptr, ctx, {}, 0};

    update_events(fd, EPOLLIN);

    // Try to complete immediately (non-blocking)
    try_complete_read(fd);
}

void EpollBackend::async_sendto_impl(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) {
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

void EpollBackend::async_wait_readable_impl(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    if (tl_in_poll_) {
        // Called from a coroutine that resumed synchronously inside poll()
        // (while the mutex is held). Defer to avoid deadlock.
        tl_deferred_waits_.push_back({fd, std::move(ctx), true});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ctx->set_type(OpType::WaitReadable);
    read_ops_[fd] = PendingOp{OpType::WaitReadable, nullptr, 0, nullptr, std::move(ctx), {}, 0};
    update_events(fd, EPOLLIN);
}

void EpollBackend::async_wait_writable_impl(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    if (tl_in_poll_) {
        tl_deferred_waits_.push_back({fd, std::move(ctx), false});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ctx->set_type(OpType::WaitWritable);
    write_ops_[fd] = PendingOp{OpType::WaitWritable, nullptr, 0, nullptr, std::move(ctx), {}, 0};
    update_events(fd, EPOLLOUT);
}

void EpollBackend::process_deferred_waits() {
    // Must be called with mutex_ held. Processes thread-local deferred waits.
    for (auto& dw : tl_deferred_waits_) {
        if (dw.readable) {
            dw.ctx->set_type(OpType::WaitReadable);
            read_ops_[dw.fd] = PendingOp{OpType::WaitReadable, nullptr, 0, nullptr, std::move(dw.ctx), {}, 0};
            update_events(dw.fd, EPOLLIN);
        } else {
            dw.ctx->set_type(OpType::WaitWritable);
            write_ops_[dw.fd] = PendingOp{OpType::WaitWritable, nullptr, 0, nullptr, std::move(dw.ctx), {}, 0};
            update_events(dw.fd, EPOLLOUT);
        }
    }
    tl_deferred_waits_.clear();
}

void EpollBackend::poll_impl(int timeout_ms) {
    // Local events array — each thread calling poll() gets its own copy on the stack.
    // This makes concurrent poll() calls from multiple threads safe.
    struct epoll_event events[MAX_EVENTS];

    // Mark that this thread is inside poll(). If a coroutine resumes synchronously
    // (within this call) and calls async_wait_readable/writable, it will defer
    // instead of trying to acquire the mutex (which would deadlock).
    tl_in_poll_ = true;

    int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

    // Collect completed contexts to resume AFTER processing all events
    // Use shared_ptr to keep OperationContext alive until after resume
    std::vector<std::shared_ptr<OperationContext>> to_resume;
    // Track map entries to erase AFTER resuming (shared_ptrs keep ctx alive)
    std::vector<socket_t> erase_reads;
    std::vector<socket_t> erase_writes;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Process deferred wait operations from coroutines that resumed in the previous poll
        process_deferred_waits();

        for (int i = 0; i < n; ++i) {
            socket_t fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            // Skip wake eventfd — just consume the data
            if (fd == wake_fd_) {
                uint64_t val;
                ::read(wake_fd_, &val, sizeof(val));
                continue;
            }

            // Handle read events
            if (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
                auto it = read_ops_.find(fd);
                if (it != read_ops_.end()) {
                    if (it->second.type == OpType::Read) {
                        ssize_t bytes = ::read(fd, it->second.buf, it->second.len);
                        if (bytes >= 0) {
                            it->second.ctx->complete(bytes, 0);
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
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
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
                            update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
                        }
                    } else if (it->second.type == OpType::Accept) {
                        socket_t new_fd = ::accept(fd, nullptr, nullptr);
                        if (new_fd != invalid_socket) {
                            set_nonblocking(new_fd);
                            *it->second.out_fd = new_fd;
                            it->second.ctx->complete(0, 0);
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx);
                            erase_reads.push_back(fd);
                        }
                    } else if (it->second.type == OpType::WaitReadable) {
                        // Socket is readable — just signal completion
                        it->second.ctx->complete(0, 0);
                        to_resume.push_back(it->second.ctx);
                        erase_reads.push_back(fd);
                        update_events(fd, write_ops_.count(fd) ? EPOLLOUT : 0);
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
                            bytes = ::write(fd, it->second.buf, it->second.len);
                        }
                        if (bytes >= 0) {
                            it->second.ctx->complete(bytes, 0);
                            to_resume.push_back(it->second.ctx);
                            erase_writes.push_back(fd);
                            update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                        } else if (!is_would_block()) {
                            it->second.ctx->complete(-1, errno);
                            to_resume.push_back(it->second.ctx);
                            erase_writes.push_back(fd);
                            update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                        }
                    } else if (it->second.type == OpType::Connect) {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        it->second.ctx->complete(err == 0 ? 0 : -1, err);
                        to_resume.push_back(it->second.ctx);
                        erase_writes.push_back(fd);
                        update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                    } else if (it->second.type == OpType::WaitWritable) {
                        // Socket is writable — just signal completion
                        it->second.ctx->complete(0, 0);
                        to_resume.push_back(it->second.ctx);
                        erase_writes.push_back(fd);
                        update_events(fd, read_ops_.count(fd) ? EPOLLIN : 0);
                    }
                }
            }
        }
    }

    // Resume all completed coroutines outside the lock.
    // The shared_ptrs in to_resume keep OperationContext alive even after
    // map entries were erased inside the locked section above.
    // We erase maps BEFORE resuming so coroutines can re-register the same fd
    // (e.g., SSL retry after WANT_READ). The shared_ptr copies in to_resume
    // prevent use-after-free.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto fd : erase_reads) read_ops_.erase(fd);
        for (auto fd : erase_writes) write_ops_.erase(fd);
    }

    for (auto& ctx : to_resume) {
        ctx->resume();
    }

    // Process any deferred waits from coroutines that resumed synchronously
    // (e.g., SSL WANT_READ/WANT_WRITE). Must acquire the mutex here.
    if (!tl_deferred_waits_.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        process_deferred_waits();
    }

    tl_in_poll_ = false;
}

void EpollBackend::wake_impl() {
    if (wake_fd_ >= 0) {
        uint64_t val = 1;
        auto r = ::write(wake_fd_, &val, sizeof(val));
        (void)r; // ignore errors (EAGAIN is fine)
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
        ssize_t n = ::read(fd, it->second.buf, it->second.len);
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
        n = ::write(fd, it->second.buf, it->second.len);
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
