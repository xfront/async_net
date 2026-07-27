#pragma once

#ifdef ASYNC_NET_LINUX

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

class EpollBackend : public IoBackendBase<EpollBackend> {
public:
    EpollBackend();
    ~EpollBackend() override;

    EpollBackend(const EpollBackend&) = delete;
    EpollBackend& operator=(const EpollBackend&) = delete;

    void poll_impl(int timeout_ms);
    bool register_impl(socket_t fd);
    void deregister_impl(socket_t fd);
    void async_read_impl(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx);
    void async_write_impl(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx);
    void async_accept_impl(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx);
    void async_connect_impl(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx);
    void async_recvfrom_impl(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx);
    void async_sendto_impl(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx);
    void async_wait_readable_impl(socket_t fd, std::shared_ptr<OperationContext> ctx);
    void async_wait_writable_impl(socket_t fd, std::shared_ptr<OperationContext> ctx);
    void wake_impl();

    static constexpr bool concurrent_poll = false;
    static constexpr const char* backend_name = "epoll";

private:
    struct PendingOp {
        OpType type;
        void* buf;
        size_t len;
        socket_t* out_fd;  // For accept
        std::shared_ptr<OperationContext> ctx;
        struct sockaddr_storage dest_addr;  // For sendto
        socklen_t dest_len;                 // For sendto
    };

    void update_events(socket_t fd, uint32_t events);
    void try_complete_read(socket_t fd);
    void try_complete_write(socket_t fd);

    int epoll_fd_;
    int wake_fd_ = -1;  // eventfd for cross-thread wakeup
    std::unordered_map<socket_t, PendingOp> read_ops_;
    std::unordered_map<socket_t, PendingOp> write_ops_;
    std::unordered_map<socket_t, uint32_t> fd_events_;  // Current epoll events per fd
    std::mutex mutex_;

    // Thread-local deferred waits: when a coroutine resumes synchronously inside
    // poll() (while the mutex is held) and calls async_wait_readable/writable,
    // we can't acquire the mutex (would deadlock). Instead, defer the operation.
    struct DeferredWait {
        socket_t fd;
        std::shared_ptr<OperationContext> ctx;
        bool readable;
    };
    static thread_local std::vector<DeferredWait> tl_deferred_waits_;
    static thread_local bool tl_in_poll_;

    void process_deferred_waits();  // Process tl_deferred_waits_ with mutex held

    static constexpr int MAX_EVENTS = 1024;
    // NOTE: events array is intentionally NOT a class member.
    // poll() uses a local stack array to allow concurrent calls from multiple threads.
};

} // namespace async_net

#endif // ASYNC_NET_LINUX
