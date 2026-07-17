#include <async_net/detail/config.hpp>

#if defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)

#include "kqueue_backend.hpp"
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace async_net {

KqueueBackend::KqueueBackend() {
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        throw std::runtime_error("Failed to create kqueue fd");
    }
}

KqueueBackend::~KqueueBackend() {
    if (kqueue_fd_ >= 0) {
        ::close(kqueue_fd_);
    }
}

bool KqueueBackend::register_socket(socket_t fd) {
    if (set_nonblocking(fd) != 0) {
        return false;
    }
    return true;
}

void KqueueBackend::deregister_socket(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove events
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, ev, 2, nullptr, 0, nullptr);

    read_ops_.erase(fd);
    write_ops_.erase(fd);
}

void KqueueBackend::async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::Read, buf, len, nullptr, ctx};

    register_event(fd, EVFILT_READ);
}

void KqueueBackend::async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    write_ops_[fd] = PendingOp{OpType::Write, const_cast<void*>(buf), len, nullptr, ctx};

    register_event(fd, EVFILT_WRITE);
}

void KqueueBackend::async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[listen_fd] = PendingOp{OpType::Accept, nullptr, 0, out_fd, ctx};

    register_event(listen_fd, EVFILT_READ);
}

void KqueueBackend::async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) {
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

    write_ops_[fd] = PendingOp{OpType::Connect, nullptr, 0, nullptr, ctx};
    register_event(fd, EVFILT_WRITE);
}

void KqueueBackend::poll(int timeout_ms) {
    struct timespec ts;
    struct timespec* ts_ptr = nullptr;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    int n = kevent(kqueue_fd_, nullptr, 0, events_, MAX_EVENTS, ts_ptr);

    std::lock_guard<std::mutex> lock(mutex_);

    for (int i = 0; i < n; ++i) {
        socket_t fd = events_[i].ident;
        short filter = events_[i].filter;

        if (filter == EVFILT_READ) {
            auto it = read_ops_.find(fd);
            if (it != read_ops_.end()) {
                if (it->second.type == OpType::Read) {
                    try_complete_read(fd);
                } else if (it->second.type == OpType::Accept) {
                    socket_t new_fd = ::accept(fd, nullptr, nullptr);
                    if (new_fd != invalid_socket) {
                        set_nonblocking(new_fd);
                        *it->second.out_fd = new_fd;
                        it->second.ctx->complete(0, 0);
                        read_ops_.erase(it);
                    } else if (!is_would_block()) {
                        it->second.ctx->complete(-1, errno);
                        read_ops_.erase(it);
                    }
                }
            }
        } else if (filter == EVFILT_WRITE) {
            auto it = write_ops_.find(fd);
            if (it != write_ops_.end()) {
                if (it->second.type == OpType::Write) {
                    try_complete_write(fd);
                } else if (it->second.type == OpType::Connect) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    it->second.ctx->complete(err == 0 ? 0 : -1, err);
                    write_ops_.erase(it);
                }
            }
        }
    }
}

void KqueueBackend::register_event(socket_t fd, short filter) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
}

void KqueueBackend::try_complete_read(socket_t fd) {
    auto it = read_ops_.find(fd);
    if (it == read_ops_.end()) return;

    ssize_t n = ::recv(fd, it->second.buf, it->second.len, 0);
    if (n >= 0) {
        it->second.ctx->complete(n, 0);
        read_ops_.erase(it);
    } else if (!is_would_block()) {
        it->second.ctx->complete(-1, errno);
        read_ops_.erase(it);
    } else {
        // Re-register for next time
        register_event(fd, EVFILT_READ);
    }
}

void KqueueBackend::try_complete_write(socket_t fd) {
    auto it = write_ops_.find(fd);
    if (it == write_ops_.end()) return;

    ssize_t n = ::send(fd, it->second.buf, it->second.len, 0);
    if (n >= 0) {
        it->second.ctx->complete(n, 0);
        write_ops_.erase(it);
    } else if (!is_would_block()) {
        it->second.ctx->complete(-1, errno);
        write_ops_.erase(it);
    } else {
        // Re-register for next time
        register_event(fd, EVFILT_WRITE);
    }
}

} // namespace async_net

#endif // ASYNC_NET_MACOS || ASYNC_NET_BSD
