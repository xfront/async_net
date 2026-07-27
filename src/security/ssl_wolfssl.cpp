// wolfSSL SSL backend policy — replaces free functions with WolfSslPolicy static methods

#include "ssl_backend.hpp"

#ifdef ASYNC_NET_SSL_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/openssl/asn1.h>
#include <cstdio>
#include <cstring>

namespace async_net::ssl {

// ============================================================================
// WolfSslPolicy — static methods wrapping wolfSSL API
// ============================================================================

void WolfSslPolicy::init() {
    static bool done = false;
    if (!done) { wolfSSL_Init(); done = true; }
}

void WolfSslPolicy::drain_errors(const char* prefix) {
    unsigned long sslerr;
    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    while ((sslerr = wolfSSL_ERR_get_error()) != 0) {
        wolfSSL_ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[%s] %s\n", prefix, errbuf);
    }
    std::fflush(stderr);
}

static WOLFSSL_METHOD* wolf_method(const std::string& mstr) {
    if (mstr == "tls_server")  return wolfSSLv23_server_method();
    if (mstr == "tls_client")  return wolfSSLv23_client_method();
    if (mstr == "dtls_server") return wolfDTLS_server_method();
    if (mstr == "dtls_client") return wolfDTLS_client_method();
    if (mstr == "dtls" || mstr == "dtls_peer") return wolfDTLS_method();
    return wolfSSLv23_method();
}

WolfSslPolicy::ctx_type* WolfSslPolicy::ctx_new(const char* method) {
    return wolfSSL_CTX_new(wolf_method(method));
}

void WolfSslPolicy::ctx_free(ctx_type* ctx) {
    wolfSSL_CTX_free(ctx);
}

bool WolfSslPolicy::ctx_use_cert(ctx_type* ctx, const char* path) {
    return wolfSSL_CTX_use_certificate_chain_file(ctx, path) == WOLFSSL_SUCCESS;
}

bool WolfSslPolicy::ctx_use_key(ctx_type* ctx, const char* path) {
    return wolfSSL_CTX_use_PrivateKey_file(ctx, path, WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS;
}

bool WolfSslPolicy::ctx_load_verify(ctx_type* ctx, const char* path) {
    return wolfSSL_CTX_load_verify_locations(ctx, path, nullptr) == WOLFSSL_SUCCESS;
}

void WolfSslPolicy::ctx_set_cipher_list(ctx_type* ctx, const char* ciphers) {
    wolfSSL_CTX_set_cipher_list(ctx, ciphers);
}

void WolfSslPolicy::ctx_set_verify(ctx_type* ctx, bool verify) {
    wolfSSL_CTX_set_verify(ctx,
        verify ? WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT : WOLFSSL_VERIFY_NONE,
        nullptr);
}

void WolfSslPolicy::ctx_set_alpn_protos(ctx_type* ctx, const unsigned char* wire, unsigned int len) {
    wolfSSL_CTX_set_alpn_protos(ctx, wire, len);
}

static int wolf_alpn_select_cb(WOLFSSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
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

void WolfSslPolicy::ctx_set_alpn_select_cb(ctx_type* ctx,
    std::function<std::string(const std::vector<std::string>&)>* user_cb) {
    wolfSSL_CTX_set_alpn_select_cb(ctx, wolf_alpn_select_cb, user_cb);
}

WolfSslPolicy::ssl_type* WolfSslPolicy::stream_new(ctx_type* ctx, int fd) {
    auto* s = wolfSSL_new(ctx);
    if (s) wolfSSL_set_fd(s, fd);
    return s;
}

void WolfSslPolicy::stream_free(ssl_type* ssl) {
    wolfSSL_free(ssl);
}

void WolfSslPolicy::stream_set_accept_state(ssl_type* ssl) {
    wolfSSL_set_accept_state(ssl);
}

void WolfSslPolicy::stream_set_connect_state(ssl_type* ssl) {
    wolfSSL_set_connect_state(ssl);
}

int WolfSslPolicy::stream_do_handshake(ssl_type* ssl) {
    return wolfSSL_SSL_do_handshake(ssl);
}

int WolfSslPolicy::stream_read(ssl_type* ssl, void* buf, int len) {
    return wolfSSL_read(ssl, buf, len);
}

int WolfSslPolicy::stream_write(ssl_type* ssl, const void* buf, int len) {
    return wolfSSL_write(ssl, buf, len);
}

int WolfSslPolicy::stream_shutdown(ssl_type* ssl) {
    return wolfSSL_shutdown(ssl);
}

int WolfSslPolicy::stream_get_error(ssl_type* ssl, int ret) {
    int err = wolfSSL_get_error(ssl, ret);
    if (err == WOLFSSL_ERROR_NONE)        return ERR_NONE;
    if (err == WOLFSSL_ERROR_WANT_READ)   return ERR_WANT_READ;
    if (err == WOLFSSL_ERROR_WANT_WRITE)  return ERR_WANT_WRITE;
    if (err == WOLFSSL_ERROR_ZERO_RETURN) return ERR_ZERO_RETURN;
    return err;
}

std::string WolfSslPolicy::stream_alpn_selected(ssl_type* ssl) {
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    wolfSSL_get0_alpn_selected(ssl, &proto, &len);
    if (proto && len > 0) {
        return std::string(reinterpret_cast<const char*>(proto), len);
    }
    return {};
}

} // namespace async_net::ssl

#endif // ASYNC_NET_SSL_WOLFSSL
