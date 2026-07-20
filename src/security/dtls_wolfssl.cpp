// wolfSSL DTLS backend implementation — custom I/O callbacks

#include "dtls_backend.hpp"

#ifdef ASYNC_NET_SSL_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace async_net::net::dtls_backend {

// I/O context for wolfSSL custom callbacks
struct io_ctx {
    int fd;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    bool has_peer_addr;
};

struct dtls_handle {
    WOLFSSL* ssl;
    io_ctx* io;
    bool is_server;
    bool last_want_read;
    bool last_want_write;
};

static int io_recv_callback(WOLFSSL*, char* buf, int sz, void* ctx) {
    auto* io = static_cast<io_ctx*>(ctx);
    struct sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);
    ssize_t n = ::recvfrom(io->fd, buf, static_cast<size_t>(sz), 0,
                           reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
    if (n > 0) {
        if (!io->has_peer_addr) {
            std::memcpy(&io->peer_addr, &from_addr, sizeof(from_addr));
            io->peer_addr_len = from_len;
            io->has_peer_addr = true;
        }
        return static_cast<int>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return WOLFSSL_CBIO_ERR_WANT_READ;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int io_send_callback(WOLFSSL*, char* buf, int sz, void* ctx) {
    auto* io = static_cast<io_ctx*>(ctx);
    ssize_t n;
    if (io->has_peer_addr) {
        n = ::sendto(io->fd, buf, static_cast<size_t>(sz), 0,
                     reinterpret_cast<const struct sockaddr*>(&io->peer_addr),
                     io->peer_addr_len);
    } else {
        // Fallback: use send() for pre-connected UDP sockets
        n = ::send(io->fd, buf, static_cast<size_t>(sz), 0);
    }
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

dtls_handle* create(void* ctx_handle, int fd, bool is_server) {
    auto* h = new dtls_handle();
    h->is_server = is_server;
    h->last_want_read = false;
    h->last_want_write = false;

    auto* io = new io_ctx();
    std::memset(io, 0, sizeof(io_ctx));
    io->fd = fd;
    io->has_peer_addr = false;
    h->io = io;

    auto* wctx = static_cast<WOLFSSL_CTX*>(ctx_handle);
    wolfSSL_CTX_SetIORecv(wctx, io_recv_callback);
    wolfSSL_CTX_SetIOSend(wctx, io_send_callback);

    auto* wssl = wolfSSL_new(wctx);
    if (wssl) {
        wolfSSL_SetIOReadCtx(wssl, io);
        wolfSSL_SetIOWriteCtx(wssl, io);
    }
    h->ssl = wssl;
    return h;
}

void destroy(dtls_handle* h) {
    if (!h) return;
    if (h->ssl) wolfSSL_free(h->ssl);
    delete h->io;
    delete h;
}

dtls_handle* move(dtls_handle* other) {
    // Just return other — the caller transfers ownership
    return other;
}

void begin_handshake(dtls_handle* h) {
    if (!h || !h->ssl) return;
    if (h->is_server) wolfSSL_set_accept_state(h->ssl);
    else              wolfSSL_set_connect_state(h->ssl);
}

static void track_want(dtls_handle* h, int err) {
    h->last_want_read = (err == WOLFSSL_ERROR_WANT_READ);
    h->last_want_write = (err == WOLFSSL_ERROR_WANT_WRITE);
}

int handshake_step(dtls_handle* h) {
    if (!h || !h->ssl) return ERROR;

    int ret = wolfSSL_SSL_do_handshake(h->ssl);
    int err = wolfSSL_get_error(h->ssl, ret);

    if (err == WOLFSSL_ERROR_NONE) {
        h->last_want_read = h->last_want_write = false;
        return OK;
    }
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        track_want(h, err);
        return (err == WOLFSSL_ERROR_WANT_READ) ? WANT_READ : WANT_WRITE;
    }

    track_want(h, err);
    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string_n(err, errbuf, sizeof(errbuf));
    std::fprintf(stderr, "[dtls] handshake FATAL: %s (err=%d)\n", errbuf, err);
    return ERROR;
}

int handshake(dtls_handle* h, int fd) {
    begin_handshake(h);

    constexpr int MAX_ATTEMPTS = 60;
    for (int i = 0; i < MAX_ATTEMPTS; ++i) {
        int ret = handshake_step(h);
        if (ret == OK) return OK;
        if (ret != WANT_READ && ret != WANT_WRITE) return ERROR;

        // Wait for socket readiness before retrying to avoid busy-loop
        // (SSL lib may not re-invoke I/O callbacks if called immediately)
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(fd, &rfds);
        if (ret == WANT_WRITE) FD_SET(fd, &wfds);
        struct timeval tv = {1, 0};
        int nfds = fd + 1;
        int sel = ::select(nfds, &rfds, (ret == WANT_WRITE) ? &wfds : nullptr, nullptr, &tv);
        if (sel < 0) return ERROR;
    }
    return ERROR;
}

int set_peer_from_socket(dtls_handle* h, int fd) {
    struct sockaddr_in peer_addr{};
    socklen_t addr_len = sizeof(peer_addr);
    uint8_t peek_buf[1];
    ssize_t n = ::recvfrom(fd, peek_buf, 1, MSG_PEEK,
                           reinterpret_cast<struct sockaddr*>(&peer_addr), &addr_len);
    if (n <= 0) return ERROR;

    std::memcpy(&h->io->peer_addr, &peer_addr, sizeof(peer_addr));
    h->io->peer_addr_len = addr_len;
    h->io->has_peer_addr = true;
    return OK;
}

int read(dtls_handle* h, void* buf, size_t len) {
    if (!h || !h->ssl) return ERROR;

    int ret = wolfSSL_read(h->ssl, buf, static_cast<int>(len));
    if (ret > 0) {
        h->last_want_read = h->last_want_write = false;
        return ret;
    }

    int err = wolfSSL_get_error(h->ssl, ret);
    if (err == WOLFSSL_ERROR_WANT_READ) {
        track_want(h, err);
        return 0;
    }
    track_want(h, err);
    return ERROR;
}

int write(dtls_handle* h, const void* buf, size_t len) {
    if (!h || !h->ssl) return ERROR;

    int ret = wolfSSL_write(h->ssl, buf, static_cast<int>(len));
    if (ret > 0) {
        h->last_want_read = h->last_want_write = false;
        return ret;
    }

    int err = wolfSSL_get_error(h->ssl, ret);
    if (err == WOLFSSL_ERROR_WANT_WRITE) {
        track_want(h, err);
        return 0;
    }
    track_want(h, err);
    return ERROR;
}

void shutdown(dtls_handle* h) {
    if (h && h->ssl) wolfSSL_shutdown(h->ssl);
}

int set_peer(dtls_handle* h, const char* ip, uint16_t port) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return ERROR;

    std::memcpy(&h->io->peer_addr, &addr, sizeof(addr));
    h->io->peer_addr_len = sizeof(addr);
    h->io->has_peer_addr = true;
    return OK;
}

bool wants_read(const dtls_handle* h) { return h ? h->last_want_read : false; }
bool wants_write(const dtls_handle* h) { return h ? h->last_want_write : false; }

} // namespace async_net::net::dtls_backend

#endif // ASYNC_NET_SSL_WOLFSSL
