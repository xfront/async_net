#pragma once

#ifdef ASYNC_NET_WINDOWS

#include <async_net/io/io_backend.hpp>
#include <async_net/io/operation_context.hpp>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace async_net {

class IocpBackend : public IoBackend {
public:
    IocpBackend();
    ~IocpBackend() override;

    IocpBackend(const IocpBackend&) = delete;
    IocpBackend& operator=(const IocpBackend&) = delete;

    void poll(int timeout_ms) override;
    bool register_socket(socket_t fd) override;
    void deregister_socket(socket_t fd) override;
    void async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) override;
    void async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) override;
    void async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) override;
    void async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) override;

    const char* name() const override { return "iocp"; }

private:
    // Overlapped operation structure
    struct OverlappedOp {
        OVERLAPPED overlapped;
        OpType type;
        void* buf;
        size_t len;
        socket_t* out_fd;
        std::shared_ptr<OperationContext> ctx;
        socket_t fd;

        OverlappedOp() {
            memset(&overlapped, 0, sizeof(overlapped));
        }
    };

    HANDLE iocp_handle_;
    std::unordered_map<socket_t, std::vector<std::unique_ptr<OverlappedOp>>> pending_ops_;
    std::mutex mutex_;

    static constexpr DWORD MAX_COMPLETIONS = 1024;
};

} // namespace async_net

#endif // ASYNC_NET_WINDOWS
