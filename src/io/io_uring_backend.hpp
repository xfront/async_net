#pragma once

#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_LINUX

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <linux/io_uring.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

// Pure io_uring backend using raw syscalls (zero external dependencies).
// Requires Linux kernel 5.1+ for basic operations, 5.11+ for full socket support.
// Natively supports: recv, send, accept, connect — no epoll fallback needed.
class IoUringBackend : public IoBackend {
public:
    explicit IoUringBackend(unsigned ring_size = 256);
    ~IoUringBackend() override;

    IoUringBackend(const IoUringBackend&) = delete;
    IoUringBackend& operator=(const IoUringBackend&) = delete;

    void poll(int timeout_ms) override;
    bool register_socket(socket_t fd) override;
    void deregister_socket(socket_t fd) override;
    void async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) override;
    void async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) override;
    void async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) override;
    void async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) override;
    void async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) override;
    void async_sendto(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) override;
    void async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) override;
    void async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) override;
    void wake() override;

    const char* name() const override { return "io_uring"; }

private:
    struct PendingOp {
        OpType type;
        std::shared_ptr<OperationContext> ctx;
        socket_t* out_fd;                     // For accept
        struct sockaddr_storage addr_storage;  // For accept/connect/recvfrom/sendto
        socklen_t addr_len;                    // For accept/connect/sendto
        void* buf;                             // For recvmsg/sendmsg
        size_t len;
        struct iovec iov;                      // For recvmsg/sendmsg
        struct msghdr msg;                     // For recvmsg/sendmsg
    };

    // Syscall wrappers
    static int io_uring_setup(unsigned entries, struct io_uring_params* p);
    static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags);

    // Ring setup
    void setup_ring(unsigned ring_size);
    void unmap_ring();

    // SQE management
    struct io_uring_sqe* get_sqe();
    void submit_sqes();

    // CQE processing (must be called with mutex_ held)
    void process_cqes(std::vector<OperationContext*>& to_resume);

    // Ring fd (from io_uring_setup)
    int ring_fd_;
    int wake_fd_ = -1;  // eventfd for cross-thread wakeup
    static constexpr __u64 WAKEUP_USER_DATA = ~static_cast<__u64>(0);

    // Submission queue pointers (mmap'd)
    unsigned* sq_head_;
    unsigned* sq_tail_;
    unsigned* sq_ring_mask_ptr_;
    unsigned* sq_ring_entries_ptr_;
    unsigned* sq_flags_;
    unsigned* sq_array_;
    struct io_uring_sqe* sqes_;

    // Completion queue pointers (mmap'd)
    unsigned* cq_head_;
    unsigned* cq_tail_;
    unsigned* cq_ring_mask_ptr_;
    unsigned* cq_ring_entries_ptr_;
    struct io_uring_cqe* cqes_;

    // mmap regions for cleanup
    void* sq_ring_ptr_;
    size_t sq_ring_sz_;
    void* cq_ring_ptr_;
    size_t cq_ring_sz_;
    void* sqes_ptr_;
    size_t sqes_sz_;

    // Local copies of ring parameters
    unsigned sq_mask_;
    unsigned cq_mask_;
    unsigned sq_entries_;
    unsigned cq_entries_;

    // Pending operations tracked by fd
    std::unordered_map<socket_t, PendingOp> read_ops_;
    std::unordered_map<socket_t, PendingOp> write_ops_;
    std::mutex mutex_;
};

} // namespace async_net

#endif // ASYNC_NET_LINUX
