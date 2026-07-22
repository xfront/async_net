// OpenSSL-compatible DTLS backend implementation (AWS-LC / LibreSSL) — BIO-based

#include "dtls_backend.hpp"

#if defined(ASYNC_NET_SSL_AWSLC) || defined(ASYNC_NET_SSL_LIBRESSL)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <async_net/detail/platform.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <sys/select.h>
#endif
#include <cstdio>
#include <cstring>

namespace async_net::net::dtls_backend {

struct dtls_handle {
    SSL* ssl;
    bool is_server;
    bool last_want_read;
    bool last_want_write;
};

dtls_handle* create(void* ctx_handle, int fd, bool is_server) {
    auto* h = new dtls_handle();
    h->is_server = is_server;
    h->last_want_read = false;
    h->last_want_write = false;

    BIO* bio = BIO_new_dgram(fd, BIO_NOCLOSE);
    if (!bio) { delete h; return nullptr; }

    auto* sctx = static_cast<SSL_CTX*>(ctx_handle);
    auto* s = SSL_new(sctx);
    if (s) {
        SSL_set_bio(s, bio, bio); // BIO owned by SSL
    }
    h->ssl = s;
    return h;
}

void destroy(dtls_handle* h) {
    if (!h) return;
    if (h->ssl) SSL_free(h->ssl); // SSL_free also frees BIO
    delete h;
}

dtls_handle* move(dtls_handle* other) {
    return other;
}

void begin_handshake(dtls_handle* h) {
    if (!h || !h->ssl) return;
    if (h->is_server) SSL_set_accept_state(h->ssl);
    else              SSL_set_connect_state(h->ssl);
}

static void track_want(dtls_handle* h, int err) {
    h->last_want_read = (err == SSL_ERROR_WANT_READ);
    h->last_want_write = (err == SSL_ERROR_WANT_WRITE);
}

int handshake_step(dtls_handle* h) {
    if (!h || !h->ssl) return ERROR;

    int ret = SSL_do_handshake(h->ssl);
    int err = SSL_get_error(h->ssl, ret);

    if (err == SSL_ERROR_NONE) {
        h->last_want_read = h->last_want_write = false;
        return OK;
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        track_want(h, err);
        return (err == SSL_ERROR_WANT_READ) ? WANT_READ : WANT_WRITE;
    }

    track_want(h, err);
    unsigned long sslerr = ERR_get_error();
    char errbuf[256];
    ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
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
        int sel = platform::socket_select(fd + 1, &rfds, (ret == WANT_WRITE) ? &wfds : nullptr, nullptr, &tv);
        if (sel < 0) return ERROR;
    }
    return ERROR;
}

int set_peer_from_socket(dtls_handle* h, int fd) {
    struct sockaddr_in peer_addr{};
    socklen_t addr_len = sizeof(peer_addr);
    uint8_t peek_buf[1];
    ssize_t n = platform::socket_recvfrom(fd, peek_buf, 1, MSG_PEEK,
                                          reinterpret_cast<struct sockaddr*>(&peer_addr), &addr_len);
    if (n <= 0) return ERROR;

    BIO* bio = SSL_get_wbio(h->ssl);
    if (!bio) return ERROR;
    BIO_dgram_set_peer(bio, reinterpret_cast<struct sockaddr*>(&peer_addr));
    return OK;
}

int read(dtls_handle* h, void* buf, size_t len) {
    if (!h || !h->ssl) return ERROR;

    int ret = SSL_read(h->ssl, buf, static_cast<int>(len));
    if (ret > 0) {
        h->last_want_read = h->last_want_write = false;
        return ret;
    }

    int err = SSL_get_error(h->ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        track_want(h, err);
        return 0;
    }
    track_want(h, err);
    return ERROR;
}

int write(dtls_handle* h, const void* buf, size_t len) {
    if (!h || !h->ssl) return ERROR;

    int ret = SSL_write(h->ssl, buf, static_cast<int>(len));
    if (ret > 0) {
        h->last_want_read = h->last_want_write = false;
        return ret;
    }

    int err = SSL_get_error(h->ssl, ret);
    if (err == SSL_ERROR_WANT_WRITE) {
        track_want(h, err);
        return 0;
    }
    track_want(h, err);
    return ERROR;
}

void shutdown(dtls_handle* h) {
    if (h && h->ssl) SSL_shutdown(h->ssl);
}

int set_peer(dtls_handle* h, const char* ip, uint16_t port) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return ERROR;

    BIO* bio = SSL_get_wbio(h->ssl);
    if (!bio) return ERROR;
    BIO_dgram_set_peer(bio, reinterpret_cast<const struct sockaddr*>(&addr));
    return OK;
}

bool wants_read(const dtls_handle* h) { return h ? h->last_want_read : false; }
bool wants_write(const dtls_handle* h) { return h ? h->last_want_write : false; }

} // namespace async_net::net::dtls_backend

#endif // ASYNC_NET_SSL_AWSLC || ASYNC_NET_SSL_LIBRESSL
