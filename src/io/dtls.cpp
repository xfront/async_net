#include <async_net/net/dtls.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace async_net {
namespace net {

// ---------------------------------------------------------------------------
// Custom I/O callbacks — bridge between wolfSSL and non-blocking UDP socket
// ---------------------------------------------------------------------------

int dtls_stream::io_recv_callback(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    auto* io = static_cast<io_ctx*>(ctx);

    struct sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);
    ssize_t n = ::recvfrom(io->fd, buf, static_cast<size_t>(sz), 0,
                           reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
    if (n > 0) {
        // For server: learn peer address from first packet
        if (!io->has_peer_addr) {
            std::memcpy(&io->peer_addr, &from_addr, sizeof(from_addr));
            io->peer_addr_len = from_len;
            io->has_peer_addr = true;
        }
        return static_cast<int>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    return WOLFSSL_CBIO_ERR_GENERAL;
}

int dtls_stream::io_send_callback(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    auto* io = static_cast<io_ctx*>(ctx);

    if (!io->has_peer_addr) {
        std::fprintf(stderr, "[dtls:io] send callback: no peer address!\n");
        return WOLFSSL_CBIO_ERR_GENERAL;
    }

    ssize_t n = ::sendto(io->fd, buf, static_cast<size_t>(sz), 0,
                         reinterpret_cast<const struct sockaddr*>(&io->peer_addr),
                         io->peer_addr_len);
    if (n > 0) {
        return static_cast<int>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }
    return WOLFSSL_CBIO_ERR_GENERAL;
}

// ---------------------------------------------------------------------------
// dtls_stream implementation
// ---------------------------------------------------------------------------

dtls_stream::dtls_stream(int fd, ssl::context& ctx, bool is_server)
    : fd_(fd), is_server_(is_server)
{
    // Initialize I/O context
    std::memset(&io_ctx_, 0, sizeof(io_ctx_));
    io_ctx_.fd = fd;
    io_ctx_.has_peer_addr = false;

    auto* wctx = static_cast<WOLFSSL_CTX*>(ctx.native_handle());

    // Set custom I/O callbacks on the CTX (shared by all SSL objects from this CTX).
    // This is safe to call multiple times — it just overwrites with the same callbacks.
    wolfSSL_CTX_SetIORecv(wctx, io_recv_callback);
    wolfSSL_CTX_SetIOSend(wctx, io_send_callback);

    auto* wssl = wolfSSL_new(wctx);
    if (wssl) {
        // Set per-SSL I/O context — each dtls_stream has its own io_ctx
        wolfSSL_SetIOReadCtx(wssl, &io_ctx_);
        wolfSSL_SetIOWriteCtx(wssl, &io_ctx_);
    }
    ssl_ = wssl;
}

dtls_stream::~dtls_stream() {
    if (ssl_) {
        wolfSSL_free(static_cast<WOLFSSL*>(ssl_));
    }
}

dtls_stream::dtls_stream(dtls_stream&& other) noexcept
    : fd_(other.fd_)
    , ssl_(other.ssl_)
    , io_ctx_(other.io_ctx_)
    , is_server_(other.is_server_)
    , last_want_read_(other.last_want_read_)
    , last_want_write_(other.last_want_write_)
{
    other.fd_ = -1;
    other.ssl_ = nullptr;
    // Re-point wolfSSL's I/O context to our io_ctx_
    if (ssl_) {
        wolfSSL_SetIOReadCtx(static_cast<WOLFSSL*>(ssl_), &io_ctx_);
        wolfSSL_SetIOWriteCtx(static_cast<WOLFSSL*>(ssl_), &io_ctx_);
    }
}

dtls_stream& dtls_stream::operator=(dtls_stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) wolfSSL_free(static_cast<WOLFSSL*>(ssl_));
        fd_ = other.fd_;
        ssl_ = other.ssl_;
        io_ctx_ = other.io_ctx_;
        is_server_ = other.is_server_;
        last_want_read_ = other.last_want_read_;
        last_want_write_ = other.last_want_write_;
        other.fd_ = -1;
        other.ssl_ = nullptr;
        if (ssl_) {
            wolfSSL_SetIOReadCtx(static_cast<WOLFSSL*>(ssl_), &io_ctx_);
            wolfSSL_SetIOWriteCtx(static_cast<WOLFSSL*>(ssl_), &io_ctx_);
        }
    }
    return *this;
}

void dtls_stream::begin_handshake() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return;
    if (is_server_)
        wolfSSL_set_accept_state(wssl);
    else
        wolfSSL_set_connect_state(wssl);
}

void dtls_stream::track_want(int err) {
    last_want_read_ = (err == WOLFSSL_ERROR_WANT_READ);
    last_want_write_ = (err == WOLFSSL_ERROR_WANT_WRITE);
}

int dtls_stream::handshake_step() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;

    int ret = wolfSSL_SSL_do_handshake(wssl);
    int err = wolfSSL_get_error(wssl, ret);

    if (err == WOLFSSL_ERROR_NONE) {
        last_want_read_ = last_want_write_ = false;
        return 0;
    }

    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        track_want(err);
        return err;
    }

    // Fatal error
    track_want(err);
    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string_n(err, errbuf, sizeof(errbuf));
    std::fprintf(stderr, "[dtls] handshake FATAL: %s (err=%d)\n", errbuf, err);
    return err;
}

int dtls_stream::handshake() {
    begin_handshake();
    // Set socket receive timeout for DTLS retransmission
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    constexpr int MAX_ATTEMPTS = 60;
    for (int i = 0; i < MAX_ATTEMPTS; ++i) {
        int ret = handshake_step();
        if (ret == 0) return 0;
        if (ret == WOLFSSL_ERROR_WANT_READ || ret == WOLFSSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
    return -1;
}

int dtls_stream::set_peer_from_socket() {
    struct sockaddr_in peer_addr{};
    socklen_t addr_len = sizeof(peer_addr);
    uint8_t peek_buf[1];
    ssize_t n = ::recvfrom(fd_, peek_buf, 1, MSG_PEEK,
                           reinterpret_cast<struct sockaddr*>(&peer_addr), &addr_len);
    if (n <= 0) return -1;

    std::memcpy(&io_ctx_.peer_addr, &peer_addr, sizeof(peer_addr));
    io_ctx_.peer_addr_len = addr_len;
    io_ctx_.has_peer_addr = true;
    return 0;
}

int dtls_stream::read(void* buf, size_t len) {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;

    int ret = wolfSSL_read(wssl, buf, static_cast<int>(len));
    if (ret > 0) {
        last_want_read_ = last_want_write_ = false;
        return ret;
    }

    int err = wolfSSL_get_error(wssl, ret);
    if (err == WOLFSSL_ERROR_WANT_READ) {
        track_want(err);
        return 0;  // would block
    }

    track_want(err);
    return -1;  // error
}

int dtls_stream::write(const void* buf, size_t len) {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;

    int ret = wolfSSL_write(wssl, buf, static_cast<int>(len));
    if (ret > 0) {
        last_want_read_ = last_want_write_ = false;
        return ret;
    }

    int err = wolfSSL_get_error(wssl, ret);
    if (err == WOLFSSL_ERROR_WANT_WRITE) {
        track_want(err);
        return 0;  // would block
    }

    track_want(err);
    return -1;  // error
}

void dtls_stream::shutdown() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (wssl) wolfSSL_shutdown(wssl);
}

int dtls_stream::set_peer(const char* ip, uint16_t port) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return -1;

    std::memcpy(&io_ctx_.peer_addr, &addr, sizeof(addr));
    io_ctx_.peer_addr_len = sizeof(addr);
    io_ctx_.has_peer_addr = true;
    return 0;
}

} // namespace net
} // namespace async_net

#else

// Stub implementation when SSL is not available
namespace async_net {
namespace net {

dtls_stream::dtls_stream(int, ssl::context&, bool) {}
dtls_stream::~dtls_stream() {}
dtls_stream::dtls_stream(dtls_stream&&) noexcept {}
dtls_stream& dtls_stream::operator=(dtls_stream&&) noexcept { return *this; }
void dtls_stream::begin_handshake() {}
int dtls_stream::handshake_step() { return -1; }
int dtls_stream::handshake() { return -1; }
int dtls_stream::set_peer_from_socket() { return -1; }
int dtls_stream::read(void*, size_t) { return -1; }
int dtls_stream::write(const void*, size_t) { return -1; }
void dtls_stream::shutdown() {}
int dtls_stream::set_peer(const char*, uint16_t) { return -1; }

} // namespace net
} // namespace async_net

#endif // ASYNC_NET_HAS_SSL
