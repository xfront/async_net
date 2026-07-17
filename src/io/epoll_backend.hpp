#pragma once

#ifdef ASYNC_NET_LINUX

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

class EpollBackend : public IoBackend {
public:
    EpollBackend();
    ~EpollBackend() override;

    EpollBackend(const EpollBackend&) = delete;
    EpollBackend& operator=(const EpollBackend&) = delete;

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

    const char* name() const override { return "epoll"; }

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
    std::unordered_map<socket_t, PendingOp> read_ops_;
    std::unordered_map<socket_t, PendingOp> write_ops_;
    std::unordered_map<socket_t, uint32_t> fd_events_;  // Current epoll events per fd
    std::mutex mutex_;
    bool in_poll_ = false;  // Re-entrancy guard

    // Deferred wait operations (queued when called from within poll())
    struct DeferredWait {
        socket_t fd;
        std::shared_ptr<OperationContext> ctx;
        bool readable; // true=readable, false=writable
    };
    std::vector<DeferredWait> deferred_waits_;

    void process_deferred_waits(); // Must be called with mutex_ held

    static constexpr int MAX_EVENTS = 1024;
    struct epoll_event events_[MAX_EVENTS];
};

} // namespace async_net

#endif // ASYNC_NET_LINUX
