#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_LINUX

#include "io_uring_backend.hpp"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cerrno>

namespace async_net {

// ---------------------------------------------------------------------------
// Raw io_uring syscall wrappers (no liburing dependency)
// ---------------------------------------------------------------------------

int IoUringBackend::io_uring_setup(unsigned entries, struct io_uring_params* p) {
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

int IoUringBackend::io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
    return static_cast<int>(syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, nullptr, 0));
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

IoUringBackend::IoUringBackend(unsigned ring_size)
    : ring_fd_(-1),
      sq_head_(nullptr), sq_tail_(nullptr),
      sq_ring_mask_ptr_(nullptr), sq_ring_entries_ptr_(nullptr),
      sq_flags_(nullptr), sq_array_(nullptr), sqes_(nullptr),
      cq_head_(nullptr), cq_tail_(nullptr),
      cq_ring_mask_ptr_(nullptr), cq_ring_entries_ptr_(nullptr), cqes_(nullptr),
      sq_ring_ptr_(MAP_FAILED), sq_ring_sz_(0),
      cq_ring_ptr_(MAP_FAILED), cq_ring_sz_(0),
      sqes_ptr_(MAP_FAILED), sqes_sz_(0),
      sq_mask_(0), cq_mask_(0), sq_entries_(0), cq_entries_(0)
{
    setup_ring(ring_size);
}

IoUringBackend::~IoUringBackend() {
    unmap_ring();
    if (ring_fd_ >= 0) {
        ::close(ring_fd_);
    }
}

// ---------------------------------------------------------------------------
// Ring setup and teardown
// ---------------------------------------------------------------------------

void IoUringBackend::setup_ring(unsigned ring_size) {
    struct io_uring_params p{};
    ring_fd_ = io_uring_setup(ring_size, &p);
    if (ring_fd_ < 0) {
        throw std::runtime_error("io_uring_setup failed: " + std::string(strerror(errno)));
    }

    sq_entries_ = p.sq_entries;
    cq_entries_ = p.cq_entries;
    // NOTE: p.sq_off.ring_mask is the BYTE OFFSET to the mask value in the mmap'd region,
    // not the mask value itself. We read the actual values after mmap'ing.
    // sq_mask_ and cq_mask_ will be set after mmap.

    bool single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;

    size_t sq_mmap_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_mmap_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

    if (single_mmap) {
        // Use CQ-based size like the minimal working test
        size_t sz = cq_mmap_sz;

        sq_ring_ptr_ = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ring_ptr_ == MAP_FAILED) {
            throw std::runtime_error("mmap SQ ring failed");
        }
        sq_ring_sz_ = sz;

        auto* base = static_cast<char*>(sq_ring_ptr_);

        sq_head_             = reinterpret_cast<unsigned*>(base + p.sq_off.head);
        sq_tail_             = reinterpret_cast<unsigned*>(base + p.sq_off.tail);
        sq_ring_mask_ptr_    = reinterpret_cast<unsigned*>(base + p.sq_off.ring_mask);
        sq_ring_entries_ptr_ = reinterpret_cast<unsigned*>(base + p.sq_off.ring_entries);
        sq_flags_            = reinterpret_cast<unsigned*>(base + p.sq_off.flags);
        sq_array_            = reinterpret_cast<unsigned*>(base + p.sq_off.array);

        cq_head_             = reinterpret_cast<unsigned*>(base + p.cq_off.head);
        cq_tail_             = reinterpret_cast<unsigned*>(base + p.cq_off.tail);
        cq_ring_mask_ptr_    = reinterpret_cast<unsigned*>(base + p.cq_off.ring_mask);
        cq_ring_entries_ptr_ = reinterpret_cast<unsigned*>(base + p.cq_off.ring_entries);
        cqes_                = reinterpret_cast<struct io_uring_cqe*>(base + p.cq_off.cqes);

        cq_ring_ptr_ = MAP_FAILED;
        cq_ring_sz_ = 0;

        // Read actual mask and entries from mmap'd region
        sq_mask_ = *sq_ring_mask_ptr_;
        cq_mask_ = *cq_ring_mask_ptr_;
        sq_entries_ = *sq_ring_entries_ptr_;
        cq_entries_ = *cq_ring_entries_ptr_;
    } else {
        sq_ring_ptr_ = mmap(nullptr, sq_mmap_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ring_ptr_ == MAP_FAILED) {
            throw std::runtime_error("mmap SQ ring failed");
        }
        sq_ring_sz_ = sq_mmap_sz;

        auto* sq_base = static_cast<char*>(sq_ring_ptr_);
        sq_head_             = reinterpret_cast<unsigned*>(sq_base + p.sq_off.head);
        sq_tail_             = reinterpret_cast<unsigned*>(sq_base + p.sq_off.tail);
        sq_ring_mask_ptr_    = reinterpret_cast<unsigned*>(sq_base + p.sq_off.ring_mask);
        sq_ring_entries_ptr_ = reinterpret_cast<unsigned*>(sq_base + p.sq_off.ring_entries);
        sq_flags_            = reinterpret_cast<unsigned*>(sq_base + p.sq_off.flags);
        sq_array_            = reinterpret_cast<unsigned*>(sq_base + p.sq_off.array);

        cq_ring_ptr_ = mmap(nullptr, cq_mmap_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
        if (cq_ring_ptr_ == MAP_FAILED) {
            throw std::runtime_error("mmap CQ ring failed");
        }
        cq_ring_sz_ = cq_mmap_sz;

        auto* cq_base = static_cast<char*>(cq_ring_ptr_);
        cq_head_             = reinterpret_cast<unsigned*>(cq_base + p.cq_off.head);
        cq_tail_             = reinterpret_cast<unsigned*>(cq_base + p.cq_off.tail);
        cq_ring_mask_ptr_    = reinterpret_cast<unsigned*>(cq_base + p.cq_off.ring_mask);
        cq_ring_entries_ptr_ = reinterpret_cast<unsigned*>(cq_base + p.cq_off.ring_entries);
        cqes_                = reinterpret_cast<struct io_uring_cqe*>(cq_base + p.cq_off.cqes);

        // Read actual mask and entries from mmap'd regions
        sq_mask_ = *sq_ring_mask_ptr_;
        cq_mask_ = *cq_ring_mask_ptr_;
        sq_entries_ = *sq_ring_entries_ptr_;
        cq_entries_ = *cq_ring_entries_ptr_;
    }

    size_t sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    sqes_ptr_ = mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
    if (sqes_ptr_ == MAP_FAILED) {
        throw std::runtime_error("mmap SQEs failed");
    }
    sqes_sz_ = sqes_sz;
    sqes_ = static_cast<struct io_uring_sqe*>(sqes_ptr_);
}

void IoUringBackend::unmap_ring() {
    if (sqes_ptr_ != MAP_FAILED) {
        munmap(sqes_ptr_, sqes_sz_);
        sqes_ptr_ = MAP_FAILED;
    }
    if (cq_ring_ptr_ != MAP_FAILED) {
        munmap(cq_ring_ptr_, cq_ring_sz_);
        cq_ring_ptr_ = MAP_FAILED;
    }
    if (sq_ring_ptr_ != MAP_FAILED) {
        munmap(sq_ring_ptr_, sq_ring_sz_);
        sq_ring_ptr_ = MAP_FAILED;
    }
}

// ---------------------------------------------------------------------------
// SQE / submission helpers
// ---------------------------------------------------------------------------

struct io_uring_sqe* IoUringBackend::get_sqe() {
    unsigned tail = *sq_tail_;
    unsigned head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);

    if (tail - head >= sq_entries_) {
        // SQ full — must submit before we can add more
        submit_sqes();
        tail = *sq_tail_;
        head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail - head >= sq_entries_) {
            return nullptr;
        }
    }

    unsigned index = tail & sq_mask_;
    struct io_uring_sqe* sqe = &sqes_[index];
    memset(sqe, 0, sizeof(*sqe));
    sq_array_[index] = index;
    __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

void IoUringBackend::submit_sqes() {
    unsigned tail = *sq_tail_;
    unsigned head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    unsigned to_submit = tail - head;
    if (to_submit > 0) {
        io_uring_enter(ring_fd_, to_submit, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// CQE processing — called under lock, does NOT resume coroutines
// ---------------------------------------------------------------------------

void IoUringBackend::process_cqes(std::vector<OperationContext*>& to_resume) {
    unsigned head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);

    while (head != tail) {
        unsigned index = head & cq_mask_;
        struct io_uring_cqe* cqe = &cqes_[index];

        auto* ctx = reinterpret_cast<OperationContext*>(cqe->user_data);
        if (ctx) {
            int res = cqe->res;
            bool found = false;

            // Search read_ops_
            for (auto it = read_ops_.begin(); it != read_ops_.end(); ++it) {
                if (it->second.ctx.get() == ctx) {
                    if (it->second.type == OpType::Accept && it->second.out_fd) {
                        // Set output fd from io_uring result BEFORE completing
                        if (res >= 0) {
                            *it->second.out_fd = static_cast<socket_t>(res);
                            ctx->complete(0, 0);
                        } else {
                            *it->second.out_fd = invalid_socket;
                            ctx->complete(-1, -res);
                        }
                    } else if (it->second.type == OpType::RecvFrom) {
                        // Copy sender address from recvmsg
                        if (res >= 0 && it->second.msg.msg_namelen > 0) {
                            memcpy(&ctx->from_addr_, &it->second.addr_storage, it->second.msg.msg_namelen);
                            ctx->from_len_ = it->second.msg.msg_namelen;
                        }
                        ctx->complete(res >= 0 ? res : -1, res < 0 ? -res : 0);
                    } else {
                        ctx->complete(res >= 0 ? res : -1, res < 0 ? -res : 0);
                    }
                    to_resume.push_back(ctx);
                    read_ops_.erase(it);
                    found = true;
                    break;
                }
            }

            if (!found) {
                for (auto it = write_ops_.begin(); it != write_ops_.end(); ++it) {
                    if (it->second.ctx.get() == ctx) {
                        ctx->complete(res >= 0 ? res : -1, res < 0 ? -res : 0);
                        to_resume.push_back(ctx);
                        write_ops_.erase(it);
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                ctx->complete(res >= 0 ? res : -1, res < 0 ? -res : 0);
                to_resume.push_back(ctx);
            }
        }

        head++;
    }

    __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// IoBackend interface
// ---------------------------------------------------------------------------

bool IoUringBackend::register_socket(socket_t fd) {
    if (set_nonblocking(fd) != 0) {
        return false;
    }
    return true;
}

void IoUringBackend::deregister_socket(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_ops_.erase(fd);
    write_ops_.erase(fd);
}

// NOTE: async_* functions only queue SQEs. They do NOT call io_uring_enter.
// Submission and completion processing happen in poll() to avoid deadlock
// (coroutine resumption must happen outside the mutex).

void IoUringBackend::async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    read_ops_[fd] = PendingOp{OpType::Read, ctx, nullptr, {}, 0};

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        read_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = static_cast<__u32>(len);
    sqe->msg_flags = 0;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
    // SQE queued — poll() will submit it
}

void IoUringBackend::async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    write_ops_[fd] = PendingOp{OpType::Write, ctx, nullptr, {}, 0};

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        write_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(buf);
    sqe->len = static_cast<__u32>(len);
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& op = read_ops_[listen_fd];
    op.type = OpType::Accept;
    op.ctx = ctx;
    op.out_fd = out_fd;
    op.addr_len = sizeof(op.addr_storage);

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        *out_fd = invalid_socket;
        ctx->complete(-1, EBUSY);
        read_ops_.erase(listen_fd);
        return;
    }

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = listen_fd;
    sqe->addr = 0;  // We don't need peer address
    sqe->off = 0;
    sqe->accept_flags = 0;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& op = write_ops_[fd];
    op.type = OpType::Connect;
    op.ctx = ctx;
    op.out_fd = nullptr;
    memcpy(&op.addr_storage, addr, addrlen);
    op.addr_len = addrlen;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        write_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(&op.addr_storage);
    sqe->off = static_cast<__u64>(op.addr_len);
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& op = read_ops_[fd];
    op.type = OpType::RecvFrom;
    op.ctx = ctx;
    op.out_fd = nullptr;
    op.buf = buf;
    op.len = len;
    op.addr_len = sizeof(op.addr_storage);

    // Set up msghdr for recvmsg
    memset(&op.msg, 0, sizeof(op.msg));
    op.msg.msg_name = &op.addr_storage;
    op.msg.msg_namelen = sizeof(op.addr_storage);
    op.iov.iov_base = buf;
    op.iov.iov_len = len;
    op.msg.msg_iov = &op.iov;
    op.msg.msg_iovlen = 1;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        read_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(&op.msg);
    sqe->off = 0;
    sqe->msg_flags = 0;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_sendto(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& op = write_ops_[fd];
    op.type = OpType::SendTo;
    op.ctx = ctx;
    op.out_fd = nullptr;
    op.buf = const_cast<void*>(buf);
    op.len = len;
    if (to && tolen > 0 && tolen <= sizeof(op.addr_storage)) {
        memcpy(&op.addr_storage, to, tolen);
        op.addr_len = tolen;
    } else {
        op.addr_len = 0;
    }

    // Set up msghdr for sendmsg
    memset(&op.msg, 0, sizeof(op.msg));
    if (op.addr_len > 0) {
        op.msg.msg_name = &op.addr_storage;
        op.msg.msg_namelen = op.addr_len;
    }
    op.iov.iov_base = const_cast<void*>(buf);
    op.iov.iov_len = len;
    op.msg.msg_iov = &op.iov;
    op.msg.msg_iovlen = 1;

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        write_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<__u64>(&op.msg);
    sqe->off = 0;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    ctx->set_type(OpType::WaitReadable);
    read_ops_[fd] = PendingOp{OpType::WaitReadable, ctx, nullptr, {}, 0};

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        read_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll32_events = POLLIN;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    ctx->set_type(OpType::WaitWritable);
    write_ops_[fd] = PendingOp{OpType::WaitWritable, ctx, nullptr, {}, 0};

    struct io_uring_sqe* sqe = get_sqe();
    if (!sqe) {
        ctx->complete(-1, EBUSY);
        write_ops_.erase(fd);
        return;
    }

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll32_events = POLLOUT;
    sqe->user_data = reinterpret_cast<__u64>(ctx.get());
}

void IoUringBackend::poll(int timeout_ms) {
    std::vector<OperationContext*> to_resume;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Count pending SQEs
        unsigned sq_tail_val = *sq_tail_;
        unsigned sq_head_val = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        unsigned pending_sq = sq_tail_val - sq_head_val;

        // Check for existing CQEs
        unsigned cq_head_val = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
        unsigned cq_tail_val = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        bool has_cqes = (cq_head_val != cq_tail_val);

        if (pending_sq > 0 && !has_cqes && timeout_ms > 0) {
            // Submit SQEs AND wait for completions in one call
            io_uring_enter(ring_fd_, pending_sq, 1, IORING_ENTER_GETEVENTS);
        } else if (pending_sq > 0) {
            // Just submit, don't wait
            submit_sqes();
        }
        
        if (!has_cqes && pending_sq == 0 && timeout_ms > 0) {
            // No SQEs to submit, but wait for events
            io_uring_enter(ring_fd_, 0, 1, IORING_ENTER_GETEVENTS);
        }

        // Process all available CQEs
        process_cqes(to_resume);
    }

    // Resume coroutines OUTSIDE the lock
    for (auto* ctx : to_resume) {
        ctx->resume();
    }
}

} // namespace async_net

#endif // ASYNC_NET_LINUX
