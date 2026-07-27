#pragma once

#if defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <sys/event.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

class KqueueBackend : public IoBackendBase<KqueueBackend> {
public:
    KqueueBackend();
    ~KqueueBackend() override;

    KqueueBackend(const KqueueBackend&) = delete;
    KqueueBackend& operator=(const KqueueBackend&) = delete;

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

    static constexpr bool concurrent_poll = true;
    static constexpr const char* backend_name = "kqueue";

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
    static constexpr uintptr_t WAKEUP_IDENT = static_cast<uintptr_t>(-1);
    // NOTE: events array is intentionally NOT a class member.
    // poll() uses a local stack array to allow concurrent calls from multiple threads.
};

} // namespace async_net

#endif // ASYNC_NET_MACOS || ASYNC_NET_BSD
