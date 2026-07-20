// wolfSSL SSL backend implementation

#include "ssl_backend.hpp"

#ifdef ASYNC_NET_SSL_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/openssl/asn1.h>
#include <cstdio>
#include <cstring>

namespace async_net::ssl::backend {

void init() {
    static bool done = false;
    if (!done) { wolfSSL_Init(); done = true; }
}

void drain_errors(const char* prefix) {
    unsigned long sslerr;
    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    while ((sslerr = wolfSSL_ERR_get_error()) != 0) {
        wolfSSL_ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[%s] %s\n", prefix, errbuf);
    }
    std::fflush(stderr);
}

static WOLFSSL_METHOD* get_method(const std::string& mstr) {
    if (mstr == "tls_server")  return wolfSSLv23_server_method();
    if (mstr == "tls_client")  return wolfSSLv23_client_method();
    if (mstr == "dtls_server") return wolfDTLS_server_method();
    if (mstr == "dtls_client") return wolfDTLS_client_method();
    if (mstr == "dtls" || mstr == "dtls_peer") return wolfDTLS_method();
    return wolfSSLv23_method();
}

void* ctx_new(const char* method) {
    return wolfSSL_CTX_new(get_method(method));
}

void ctx_free(void* ctx) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(ctx));
}

bool ctx_use_cert(void* ctx, const char* path) {
    return wolfSSL_CTX_use_certificate_chain_file(static_cast<WOLFSSL_CTX*>(ctx), path) == WOLFSSL_SUCCESS;
}

bool ctx_use_key(void* ctx, const char* path) {
    return wolfSSL_CTX_use_PrivateKey_file(static_cast<WOLFSSL_CTX*>(ctx), path, WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS;
}

bool ctx_load_verify(void* ctx, const char* path) {
    return wolfSSL_CTX_load_verify_locations(static_cast<WOLFSSL_CTX*>(ctx), path, nullptr) == WOLFSSL_SUCCESS;
}

void ctx_set_cipher_list(void* ctx, const char* ciphers) {
    wolfSSL_CTX_set_cipher_list(static_cast<WOLFSSL_CTX*>(ctx), ciphers);
}

void ctx_set_verify(void* ctx, bool verify) {
    wolfSSL_CTX_set_verify(static_cast<WOLFSSL_CTX*>(ctx),
        verify ? WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT : WOLFSSL_VERIFY_NONE,
        nullptr);
}

void ctx_set_alpn_protos(void* ctx, const unsigned char* wire, unsigned int len) {
    wolfSSL_CTX_set_alpn_protos(static_cast<WOLFSSL_CTX*>(ctx), wire, len);
}

static int alpn_select_cb(WOLFSSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
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

void ctx_set_alpn_select_cb(void* ctx,
    std::function<std::string(const std::vector<std::string>&)>* user_cb) {
    wolfSSL_CTX_set_alpn_select_cb(static_cast<WOLFSSL_CTX*>(ctx), alpn_select_cb, user_cb);
}

void* stream_new(void* ctx, int fd) {
    auto* s = wolfSSL_new(static_cast<WOLFSSL_CTX*>(ctx));
    if (s) wolfSSL_set_fd(static_cast<WOLFSSL*>(s), fd);
    return s;
}

void stream_free(void* ssl) {
    wolfSSL_free(static_cast<WOLFSSL*>(ssl));
}

void stream_set_accept_state(void* ssl) {
    wolfSSL_set_accept_state(static_cast<WOLFSSL*>(ssl));
}

void stream_set_connect_state(void* ssl) {
    wolfSSL_set_connect_state(static_cast<WOLFSSL*>(ssl));
}

int stream_do_handshake(void* ssl) {
    return wolfSSL_SSL_do_handshake(static_cast<WOLFSSL*>(ssl));
}

int stream_read(void* ssl, void* buf, int len) {
    return wolfSSL_read(static_cast<WOLFSSL*>(ssl), buf, len);
}

int stream_write(void* ssl, const void* buf, int len) {
    return wolfSSL_write(static_cast<WOLFSSL*>(ssl), buf, len);
}

int stream_shutdown(void* ssl) {
    return wolfSSL_shutdown(static_cast<WOLFSSL*>(ssl));
}

int stream_get_error(void* ssl, int ret) {
    int err = wolfSSL_get_error(static_cast<WOLFSSL*>(ssl), ret);
    if (err == WOLFSSL_ERROR_NONE)        return ERR_NONE;
    if (err == WOLFSSL_ERROR_WANT_READ)   return ERR_WANT_READ;
    if (err == WOLFSSL_ERROR_WANT_WRITE)  return ERR_WANT_WRITE;
    if (err == WOLFSSL_ERROR_ZERO_RETURN) return ERR_ZERO_RETURN;
    return err; // other error
}

std::string stream_alpn_selected(void* ssl) {
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    wolfSSL_get0_alpn_selected(static_cast<WOLFSSL*>(ssl), &proto, &len);
    if (proto && len > 0) {
        return std::string(reinterpret_cast<const char*>(proto), len);
    }
    return {};
}

} // namespace async_net::ssl::backend

#endif // ASYNC_NET_SSL_WOLFSSL
