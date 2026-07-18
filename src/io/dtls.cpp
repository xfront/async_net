#include <async_net/net/dtls.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

#include <sys/socket.h>
#include <cstdio>
#include <cstring>

namespace async_net {
namespace net {

dtls_stream::dtls_stream(int fd, ssl::context& ctx, bool is_server)
    : fd_(fd), is_server_(is_server)
{
    auto* wctx = static_cast<WOLFSSL_CTX*>(ctx.native_handle());
    auto* wssl = wolfSSL_new(wctx);
    if (wssl) {
        wolfSSL_set_fd(wssl, fd);
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
    , is_server_(other.is_server_)
{
    other.fd_ = -1;
    other.ssl_ = nullptr;
}

dtls_stream& dtls_stream::operator=(dtls_stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) wolfSSL_free(static_cast<WOLFSSL*>(ssl_));
        fd_ = other.fd_;
        ssl_ = other.ssl_;
        is_server_ = other.is_server_;
        other.fd_ = -1;
        other.ssl_ = nullptr;
    }
    return *this;
}

int dtls_stream::handshake() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;

    if (is_server_)
        wolfSSL_set_accept_state(wssl);
    else
        wolfSSL_set_connect_state(wssl);

    // Set socket receive timeout for DTLS retransmission (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (int attempt = 0; attempt < 10; ++attempt) {
        int ret = wolfSSL_SSL_do_handshake(wssl);
        int err = wolfSSL_get_error(wssl, ret);

        if (err == WOLFSSL_ERROR_NONE) {
            // Clear timeout
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return 0;
        }

        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            // DTLS retransmission — retry
            continue;
        }

        // Fatal error
        char errbuf[WOLFSSL_MAX_ERROR_SZ];
        wolfSSL_ERR_error_string_n(err, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[DTLS handshake] error: %s\n", errbuf);
        return -1;
    }

    std::fprintf(stderr, "[DTLS handshake] timeout\n");
    return -1;
}

int dtls_stream::read(void* buf, size_t len) {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;
    int ret = wolfSSL_read(wssl, buf, static_cast<int>(len));
    return ret > 0 ? ret : -1;
}

int dtls_stream::write(const void* buf, size_t len) {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;
    int ret = wolfSSL_write(wssl, buf, static_cast<int>(len));
    return ret > 0 ? ret : -1;
}

void dtls_stream::shutdown() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (wssl) wolfSSL_shutdown(wssl);
}

int dtls_stream::set_peer_from_socket() {
    auto* wssl = static_cast<WOLFSSL*>(ssl_);
    if (!wssl) return -1;

    // Peek at the next UDP packet to get the sender's address
    struct sockaddr_in peer_addr{};
    socklen_t addr_len = sizeof(peer_addr);
    uint8_t peek_buf[1];
    ssize_t n = ::recvfrom(fd_, peek_buf, 1, MSG_PEEK,
                           reinterpret_cast<struct sockaddr*>(&peer_addr),
                           &addr_len);
    if (n <= 0) return -1;

    // Tell wolfSSL where to send DTLS responses
    wolfSSL_dtls_set_peer(wssl, &peer_addr, addr_len);
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
int dtls_stream::handshake() { return -1; }
int dtls_stream::read(void*, size_t) { return -1; }
int dtls_stream::write(const void*, size_t) { return -1; }
void dtls_stream::shutdown() {}
int dtls_stream::set_peer_from_socket() { return -1; }

} // namespace net
} // namespace async_net

#endif // ASYNC_NET_HAS_SSL
