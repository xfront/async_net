#pragma once

#if defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <sys/event.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

class KqueueBackend : public IoBackend {
public:
    KqueueBackend();
    ~KqueueBackend() override;

    KqueueBackend(const KqueueBackend&) = delete;
    KqueueBackend& operator=(const KqueueBackend&) = delete;

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

    const char* name() const override { return "kqueue"; }

private:
    struct PendingOp {
        OpType type;
        void* buf;
        size_t len;
        socket_t* out_fd;
        std::shared_ptr<OperationContext> ctx;
        // For sendto/recvfrom
        struct sockaddr_storage dest_addr;
        socklen_t dest_addr_len;
    };

    void register_event(socket_t fd, short filter, uintptr_t flags = EV_ADD | EV_ONESHOT);
    void try_complete_read(socket_t fd, std::vector<OperationContext*>& to_resume);
    void try_complete_write(socket_t fd, std::vector<OperationContext*>& to_resume);
    void try_complete_recvfrom(socket_t fd, std::vector<OperationContext*>& to_resume);
    void try_complete_sendto(socket_t fd, std::vector<OperationContext*>& to_resume);

    int kqueue_fd_;
    std::unordered_map<socket_t, PendingOp> read_ops_;
    std::unordered_map<socket_t, PendingOp> write_ops_;
    std::mutex mutex_;

    static constexpr int MAX_EVENTS = 1024;
    struct kevent events_[MAX_EVENTS];
};

} // namespace async_net

#endif // ASYNC_NET_MACOS || ASYNC_NET_BSD
