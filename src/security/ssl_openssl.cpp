// OpenSSL-compatible SSL backend policy (AWS-LC / LibreSSL)
// Replaces free functions with OpenSslPolicy static methods.

#include "ssl_backend.hpp"

#if defined(ASYNC_NET_SSL_AWSLC) || defined(ASYNC_NET_SSL_LIBRESSL)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <cstring>

namespace async_net::ssl {

// ============================================================================
// OpenSslPolicy — static methods wrapping OpenSSL-compatible API
// ============================================================================

void OpenSslPolicy::init() {
    static bool done = false;
    if (!done) { OPENSSL_init_ssl(0, nullptr); done = true; }
}

void OpenSslPolicy::drain_errors(const char* prefix) {
    unsigned long sslerr;
    char errbuf[256];
    while ((sslerr = ERR_get_error()) != 0) {
        ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[%s] %s\n", prefix, errbuf);
    }
    std::fflush(stderr);
}

static const SSL_METHOD* ossl_method(const std::string& mstr) {
    if (mstr == "tls_server")  return TLS_server_method();
    if (mstr == "tls_client")  return TLS_client_method();
    if (mstr == "dtls_server") return DTLS_server_method();
    if (mstr == "dtls_client") return DTLS_client_method();
    if (mstr == "dtls" || mstr == "dtls_peer") return DTLS_method();
    return TLS_method();
}

OpenSslPolicy::ctx_type* OpenSslPolicy::ctx_new(const char* method) {
    return SSL_CTX_new(ossl_method(method));
}

void OpenSslPolicy::ctx_free(ctx_type* ctx) {
    SSL_CTX_free(ctx);
}

bool OpenSslPolicy::ctx_use_cert(ctx_type* ctx, const char* path) {
    return SSL_CTX_use_certificate_chain_file(ctx, path) == 1;
}

bool OpenSslPolicy::ctx_use_key(ctx_type* ctx, const char* path) {
    return SSL_CTX_use_PrivateKey_file(ctx, path, SSL_FILETYPE_PEM) == 1;
}

bool OpenSslPolicy::ctx_load_verify(ctx_type* ctx, const char* path) {
    return SSL_CTX_load_verify_locations(ctx, path, nullptr) == 1;
}

void OpenSslPolicy::ctx_set_cipher_list(ctx_type* ctx, const char* ciphers) {
    SSL_CTX_set_cipher_list(ctx, ciphers);
}

void OpenSslPolicy::ctx_set_verify(ctx_type* ctx, bool verify) {
    SSL_CTX_set_verify(ctx,
        verify ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_NONE,
        nullptr);
}

void OpenSslPolicy::ctx_set_alpn_protos(ctx_type* ctx, const unsigned char* wire, unsigned int len) {
    SSL_CTX_set_alpn_protos(ctx, wire, len);
}

static int ossl_alpn_select_cb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                                const unsigned char* in, unsigned int inlen, void* arg) {
    auto* user_cb = static_cast<std::function<std::string(const std::vector<std::string>&)>*>(arg);
    if (!user_cb) return 1;

    std::vector<std::string> client_protos;
    unsigned int pos = 0;
    while (pos < inlen) {
        unsigned int len = in[pos++];
        if (pos + len > inlen) break;
        client_protos.emplace_back(reinterpret_cast<const char*>(in + pos), len);
        pos += len;
    }

    std::string selected = (*user_cb)(client_protos);
    if (selected.empty()) return 1;

    pos = 0;
    while (pos < inlen) {
        unsigned int len = in[pos];
        if (pos + 1 + len > inlen) break;
        std::string proto(reinterpret_cast<const char*>(in + pos + 1), len);
        if (proto == selected) {
            *out = in + pos + 1;
            *outlen = static_cast<unsigned char>(len);
            return 0;
        }
        pos += 1 + len;
    }
    return 1;
}

void OpenSslPolicy::ctx_set_alpn_select_cb(ctx_type* ctx,
    std::function<std::string(const std::vector<std::string>&)>* user_cb) {
    SSL_CTX_set_alpn_select_cb(ctx, ossl_alpn_select_cb, user_cb);
}

OpenSslPolicy::ssl_type* OpenSslPolicy::stream_new(ctx_type* ctx, int fd) {
    auto* s = SSL_new(ctx);
    if (s) SSL_set_fd(const_cast<SSL*>(s), fd);  // SSL_set_fd takes SSL* not const
    return s;
}

void OpenSslPolicy::stream_free(ssl_type* ssl) {
    SSL_free(ssl);
}

void OpenSslPolicy::stream_set_accept_state(ssl_type* ssl) {
    SSL_set_accept_state(ssl);
}

void OpenSslPolicy::stream_set_connect_state(ssl_type* ssl) {
    SSL_set_connect_state(ssl);
}

int OpenSslPolicy::stream_do_handshake(ssl_type* ssl) {
    return SSL_do_handshake(ssl);
}

int OpenSslPolicy::stream_read(ssl_type* ssl, void* buf, int len) {
    return SSL_read(ssl, buf, len);
}

int OpenSslPolicy::stream_write(ssl_type* ssl, const void* buf, int len) {
    return SSL_write(ssl, buf, len);
}

int OpenSslPolicy::stream_shutdown(ssl_type* ssl) {
    return SSL_shutdown(ssl);
}

int OpenSslPolicy::stream_get_error(ssl_type* ssl, int ret) {
    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_NONE)        return ERR_NONE;
    if (err == SSL_ERROR_WANT_READ)   return ERR_WANT_READ;
    if (err == SSL_ERROR_WANT_WRITE)  return ERR_WANT_WRITE;
    if (err == SSL_ERROR_ZERO_RETURN) return ERR_ZERO_RETURN;
    return err;
}

std::string OpenSslPolicy::stream_alpn_selected(ssl_type* ssl) {
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl, &proto, &len);
    if (proto && len > 0) {
        return std::string(reinterpret_cast<const char*>(proto), len);
    }
    return {};
}

} // namespace async_net::ssl

#endif // ASYNC_NET_SSL_AWSLC || ASYNC_NET_SSL_LIBRESSL
