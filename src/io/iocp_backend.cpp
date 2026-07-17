#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_WINDOWS

#include "iocp_backend.hpp"
#include <stdexcept>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace async_net {

IocpBackend::IocpBackend() {
    iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (iocp_handle_ == nullptr) {
        throw std::runtime_error("Failed to create IOCP handle");
    }
}

IocpBackend::~IocpBackend() {
    if (iocp_handle_ != nullptr) {
        CloseHandle(iocp_handle_);
    }
}

bool IocpBackend::register_socket(socket_t fd) {
    if (set_nonblocking(fd) != 0) {
        return false;
    }

    HANDLE result = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(fd),
        iocp_handle_,
        reinterpret_cast<ULONG_PTR>(fd),
        0
    );

    return result != nullptr;
}

void IocpBackend::deregister_socket(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ops_.erase(fd);
    // Note: On Windows, we can't explicitly deregister from IOCP
    // The socket will be cleaned up when closed
}

void IocpBackend::async_read(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::Read;
    op->buf = buf;
    op->len = len;
    op->ctx = ctx;
    op->fd = fd;

    DWORD bytes_read = 0;
    DWORD flags = 0;
    WSABUF wsabuf;
    wsabuf.buf = static_cast<char*>(buf);
    wsabuf.len = static_cast<ULONG>(len);

    int ret = WSARecv(fd, &wsabuf, 1, &bytes_read, &flags, &op->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_write(socket_t fd, const void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::Write;
    op->buf = const_cast<void*>(buf);
    op->len = len;
    op->ctx = ctx;
    op->fd = fd;

    DWORD bytes_written = 0;
    WSABUF wsabuf;
    wsabuf.buf = const_cast<char*>(static_cast<const char*>(buf));
    wsabuf.len = static_cast<ULONG>(len);

    int ret = WSASend(fd, &wsabuf, 1, &bytes_written, 0, &op->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_accept(socket_t listen_fd, socket_t* out_fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::Accept;
    op->out_fd = out_fd;
    op->ctx = ctx;
    op->fd = listen_fd;

    // Create a new socket for the accepted connection
    socket_t accept_fd = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (accept_fd == INVALID_SOCKET) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    // AcceptEx requires extra space for addresses
    char buf[(sizeof(struct sockaddr_in) + 16) * 2];
    DWORD bytes_received = 0;
    BOOL ret = AcceptEx(listen_fd, accept_fd, buf, 0,
                        sizeof(struct sockaddr_in) + 16,
                        sizeof(struct sockaddr_in) + 16,
                        &bytes_received, &op->overlapped);

    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(accept_fd);
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    *out_fd = accept_fd;
    pending_ops_[listen_fd].push_back(std::move(op));
}

void IocpBackend::async_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::Connect;
    op->ctx = ctx;
    op->fd = fd;

    // Bind the socket first (required for ConnectEx)
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr)) == SOCKET_ERROR) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    BOOL ret = ConnectEx(fd, addr, addrlen, nullptr, 0, nullptr, &op->overlapped);
    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::poll(int timeout_ms) {
    DWORD bytes_transferred = 0;
    ULONG_PTR completion_key = 0;
    LPOVERLAPPED overlapped = nullptr;

    BOOL ret = GetQueuedCompletionStatus(
        iocp_handle_,
        &bytes_transferred,
        &completion_key,
        &overlapped,
        timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)
    );

    if (overlapped == nullptr) {
        return;  // Timeout or error
    }

    // Get the operation from the overlapped pointer
    OverlappedOp* op = nullptr;
    socket_t fd = static_cast<socket_t>(completion_key);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_ops_.find(fd);
    if (it != pending_ops_.end()) {
        for (auto& pending : it->second) {
            if (&pending->overlapped == overlapped) {
                op = pending.get();
                break;
            }
        }
    }

    if (op == nullptr) {
        return;
    }

    DWORD error = ret ? NO_ERROR : GetLastError();

    if (op->type == OpType::Accept && error == NO_ERROR) {
        // Update the accept context
        setsockopt(*op->out_fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   reinterpret_cast<const char*>(&fd), sizeof(fd));
    } else if (op->type == OpType::Connect && error == NO_ERROR) {
        // Update the connect context
        setsockopt(op->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
    }

    op->ctx->complete(error == NO_ERROR ? static_cast<ssize_t>(bytes_transferred) : -1, error);

    // Remove the completed operation
    if (it != pending_ops_.end()) {
        auto& ops = it->second;
        ops.erase(std::remove_if(ops.begin(), ops.end(),
            [op](const std::unique_ptr<OverlappedOp>& p) { return p.get() == op; }),
            ops.end());
    }
}

} // namespace async_net

#endif // ASYNC_NET_WINDOWS
