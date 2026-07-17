#include <async_net/detail/config.hpp>

#ifdef ASYNC_NET_WINDOWS

#include "iocp_backend.hpp"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <mswsock.h>

namespace async_net {

// Helper: load ConnectEx function pointer
static LPFN_CONNECTEX load_connectex(SOCKET fd) {
    LPFN_CONNECTEX fn = nullptr;
    GUID guid = WSAID_CONNECTEX;
    DWORD bytes = 0;
    if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        return nullptr;
    }
    return fn;
}

// Helper: load AcceptEx function pointer
static LPFN_ACCEPTEX load_acceptex(SOCKET fd) {
    LPFN_ACCEPTEX fn = nullptr;
    GUID guid = WSAID_ACCEPTEX;
    DWORD bytes = 0;
    if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        return nullptr;
    }
    return fn;
}

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

    // Load AcceptEx dynamically (portable across MSVC/MinGW)
    auto pfn_acceptex = load_acceptex(listen_fd);
    if (!pfn_acceptex) {
        closesocket(accept_fd);
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    // AcceptEx requires extra space for addresses
    char buf[(sizeof(struct sockaddr_in) + 16) * 2];
    DWORD bytes_received = 0;
    BOOL ret = pfn_acceptex(listen_fd, accept_fd, buf, 0,
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

    // Load ConnectEx dynamically (portable across MSVC/MinGW)
    auto pfn_connectex = load_connectex(fd);
    if (!pfn_connectex) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    BOOL ret = pfn_connectex(fd, addr, addrlen, nullptr, 0, nullptr, &op->overlapped);
    if (ret == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_recvfrom(socket_t fd, void* buf, size_t len, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::RecvFrom;
    op->buf = buf;
    op->len = len;
    op->ctx = ctx;
    op->fd = fd;
    op->dest_addr_len = sizeof(op->dest_addr);
    op->wsa_buf.buf = static_cast<char*>(buf);
    op->wsa_buf.len = static_cast<ULONG>(len);

    DWORD flags = 0;
    DWORD bytes_received = 0;
    int ret = WSARecvFrom(fd, &op->wsa_buf, 1, &bytes_received, &flags,
                          reinterpret_cast<struct sockaddr*>(&op->dest_addr),
                          reinterpret_cast<LPINT>(&op->dest_addr_len),
                          &op->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_sendto(socket_t fd, const void* buf, size_t len, const struct sockaddr* to, socklen_t tolen, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::SendTo;
    op->buf = const_cast<void*>(buf);
    op->len = len;
    op->ctx = ctx;
    op->fd = fd;
    memcpy(&op->dest_addr, to, tolen);
    op->dest_addr_len = tolen;
    op->wsa_buf.buf = const_cast<char*>(static_cast<const char*>(buf));
    op->wsa_buf.len = static_cast<ULONG>(len);

    DWORD bytes_sent = 0;
    int ret = WSASendTo(fd, &op->wsa_buf, 1, &bytes_sent, 0,
                        reinterpret_cast<const struct sockaddr*>(&op->dest_addr),
                        op->dest_addr_len,
                        &op->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_wait_readable(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Use zero-byte overlapped WSARecv with MSG_PEEK to detect readability
    auto op = std::make_unique<OverlappedOp>();
    op->type = OpType::WaitReadable;
    op->ctx = ctx;
    op->fd = fd;
    op->buf = nullptr;
    op->len = 0;
    op->wsa_buf.buf = nullptr;
    op->wsa_buf.len = 0;

    DWORD flags = MSG_PEEK;
    DWORD bytes_recvd = 0;
    int ret = WSARecv(fd, &op->wsa_buf, 1, &bytes_recvd, &flags,
                      &op->overlapped, nullptr);
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        // Immediate error — complete with failure
        op->ctx->complete(-1, WSAGetLastError());
        return;
    }

    pending_ops_[fd].push_back(std::move(op));
}

void IocpBackend::async_wait_writable(socket_t fd, std::shared_ptr<OperationContext> ctx) {
    // For IOCP, complete immediately.
    // Sockets are almost always writable; if SSL_write returns WANT_WRITE again,
    // the coroutine will re-wait and retry.
    ctx->complete(0, 0);
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

    std::vector<OperationContext*> to_resume;
    {
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
        } else if (op->type == OpType::RecvFrom && error == NO_ERROR) {
            // Copy sender address to context
            memcpy(&op->ctx->from_addr_, &op->dest_addr, op->dest_addr_len);
            op->ctx->from_len_ = op->dest_addr_len;
        }

        op->ctx->complete(error == NO_ERROR ? static_cast<ssize_t>(bytes_transferred) : -1, error);
        to_resume.push_back(op->ctx.get());

        // Remove the completed operation
        if (it != pending_ops_.end()) {
            auto& ops = it->second;
            ops.erase(std::remove_if(ops.begin(), ops.end(),
                [op](const std::unique_ptr<OverlappedOp>& p) { return p.get() == op; }),
                ops.end());
        }
    }

    // Resume all completed coroutines outside the lock
    for (auto* ctx : to_resume) {
        ctx->resume();
    }
}

} // namespace async_net

#endif // ASYNC_NET_WINDOWS
