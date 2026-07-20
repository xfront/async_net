// wolfSSL QUIC SSL backend for HTTP/3

#include "h3_quic_backend.hpp"

#ifdef ASYNC_NET_SSL_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/quic.h>
extern "C" {
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
}
#include <cstring>
#include <iostream>

namespace async_net::http::quic_ssl {

// -----------------------------------------------------------------------
// Internal types
// -----------------------------------------------------------------------

struct ssl_ctx_handle {
    WOLFSSL_CTX* ctx;
};

struct ssl_handle {
    WOLFSSL* ssl;
};

// -----------------------------------------------------------------------
// Encryption level mapping
// -----------------------------------------------------------------------

static ngtcp2_encryption_level to_ngtcp2_level(WOLFSSL_ENCRYPTION_LEVEL lvl) {
    switch (lvl) {
        case wolfssl_encryption_initial:     return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
        case wolfssl_encryption_early_data:  return NGTCP2_ENCRYPTION_LEVEL_0RTT;
        case wolfssl_encryption_handshake:   return NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE;
        case wolfssl_encryption_application: return NGTCP2_ENCRYPTION_LEVEL_1RTT;
        default: return NGTCP2_ENCRYPTION_LEVEL_1RTT;
    }
}

// -----------------------------------------------------------------------
// QUIC TLS callbacks (wolfSSL)
// -----------------------------------------------------------------------

static int quic_set_encryption_secrets(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                                        const uint8_t* read_secret,
                                        const uint8_t* write_secret,
                                        size_t secret_len) {
    auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(wolfSSL_get_app_data(ssl));
    if (!ref) return 0;
    ngtcp2_conn* conn = ref->get_conn(ref);
    if (!conn) return 0;
    auto ngtcp2_level = to_ngtcp2_level(level);
    if (read_secret) {
        if (ngtcp2_crypto_derive_and_install_rx_key(conn, nullptr, nullptr, nullptr,
                ngtcp2_level, read_secret, secret_len) != 0) return -1;
    }
    if (write_secret) {
        if (ngtcp2_crypto_derive_and_install_tx_key(conn, nullptr, nullptr, nullptr,
                ngtcp2_level, write_secret, secret_len) != 0) return -1;
    }
    return 1;
}

static int quic_add_handshake_data(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                                    const uint8_t* data, size_t len) {
    auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(wolfSSL_get_app_data(ssl));
    if (!ref) return 0;
    ngtcp2_conn* conn = ref->get_conn(ref);
    if (!conn) return 0;
    auto rv = ngtcp2_conn_submit_crypto_data(conn, to_ngtcp2_level(level), data, len);
    if (rv != 0) {
        std::cerr << "[QUIC] submit_crypto_data err=" << ngtcp2_strerror(rv) << std::endl;
        return 0;
    }
    return 1;
}

static int quic_flush_flight(WOLFSSL*) { return 1; }

static int quic_send_alert(WOLFSSL*, WOLFSSL_ENCRYPTION_LEVEL, uint8_t) { return 1; }

static const WOLFSSL_QUIC_METHOD g_quic_method = {
    quic_set_encryption_secrets,
    quic_add_handshake_data,
    quic_flush_flight,
    quic_send_alert
};

// -----------------------------------------------------------------------
// ngtcp2 AEAD callbacks
// -----------------------------------------------------------------------

int encrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
               const ngtcp2_crypto_aead_ctx *aead_ctx,
               const uint8_t *plaintext, size_t plaintextlen,
               const uint8_t *nonce, size_t noncelen,
               const uint8_t *aad, size_t aadlen) {
    (void)aead; (void)noncelen;
    if (wolfSSL_quic_aead_encrypt(dest, static_cast<WOLFSSL_EVP_CIPHER_CTX*>(aead_ctx->native_handle),
                                   plaintext, plaintextlen,
                                   nonce, aad, aadlen) != WOLFSSL_SUCCESS) {
        return -1;
    }
    return 0;
}

int decrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
               const ngtcp2_crypto_aead_ctx *aead_ctx,
               const uint8_t *ciphertext, size_t ciphertextlen,
               const uint8_t *nonce, size_t noncelen,
               const uint8_t *aad, size_t aadlen) {
    (void)aead; (void)noncelen;
    if (wolfSSL_quic_aead_decrypt(dest, static_cast<WOLFSSL_EVP_CIPHER_CTX*>(aead_ctx->native_handle),
                                   ciphertext, ciphertextlen,
                                   nonce, aad, aadlen) != WOLFSSL_SUCCESS) {
        return -1;
    }
    return 0;
}

// -----------------------------------------------------------------------
// Context lifecycle & configuration
// -----------------------------------------------------------------------

ssl_ctx_handle* ctx_new_server() {
    wolfSSL_Init();
    auto* h = new ssl_ctx_handle();
    h->ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (h->ctx) {
        wolfSSL_CTX_set_min_proto_version(h->ctx, TLS1_3_VERSION);
        wolfSSL_CTX_set_max_proto_version(h->ctx, TLS1_3_VERSION);
    }
    return h;
}

ssl_ctx_handle* ctx_new_client() {
    wolfSSL_Init();
    auto* h = new ssl_ctx_handle();
    h->ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (h->ctx) {
        wolfSSL_CTX_set_min_proto_version(h->ctx, TLS1_3_VERSION);
        wolfSSL_CTX_set_max_proto_version(h->ctx, TLS1_3_VERSION);
    }
    return h;
}

void ctx_free(ssl_ctx_handle* ctx) {
    if (!ctx) return;
    if (ctx->ctx) wolfSSL_CTX_free(ctx->ctx);
    delete ctx;
}

bool ctx_use_cert(ssl_ctx_handle* ctx, const char* cert_file) {
    return wolfSSL_CTX_use_certificate_chain_file(ctx->ctx, cert_file) == 1;
}

bool ctx_use_key(ssl_ctx_handle* ctx, const char* key_file) {
    return wolfSSL_CTX_use_PrivateKey_file(ctx->ctx, key_file, SSL_FILETYPE_PEM) == 1;
}

void ctx_set_alpn_h3_client(ssl_ctx_handle* ctx) {
    const unsigned char alpn_h3[] = {2, 'h', '3'};
    wolfSSL_CTX_set_alpn_protos(ctx->ctx, alpn_h3, sizeof(alpn_h3));
}

void ctx_set_alpn_select_h3_server(ssl_ctx_handle* ctx) {
    wolfSSL_CTX_set_alpn_select_cb(ctx->ctx,
        [](WOLFSSL*, const unsigned char** out, unsigned char* outlen,
           const unsigned char* in, unsigned int inlen, void*) -> int {
            for (unsigned int i = 0; i < inlen; ) {
                uint8_t len = in[i];
                if (i + 1 + len > inlen) break;
                if (len == 2 && memcmp(in + i + 1, "h3", 2) == 0) {
                    *out = in + i + 1;
                    *outlen = 2;
                    return 0;
                }
                i += 1 + len;
            }
            return 1;
        }, nullptr);
}

void ctx_set_verify_none(ssl_ctx_handle* ctx) {
    wolfSSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_NONE, nullptr);
}

// -----------------------------------------------------------------------
// SSL object lifecycle & configuration
// -----------------------------------------------------------------------

ssl_handle* ssl_new(ssl_ctx_handle* ctx) {
    auto* h = new ssl_handle();
    h->ssl = wolfSSL_new(ctx->ctx);
    return h;
}

void ssl_free(ssl_handle* ssl) {
    if (!ssl) return;
    if (ssl->ssl) wolfSSL_free(ssl->ssl);
    delete ssl;
}

void ssl_set_quic(ssl_handle* ssl, void* conn_ref_ptr) {
    wolfSSL_set_app_data(ssl->ssl, conn_ref_ptr);
    wolfSSL_set_quic_method(ssl->ssl, &g_quic_method);
}

void* ssl_native_handle(ssl_handle* ssl) {
    return ssl->ssl;
}

} // namespace async_net::http::quic_ssl

#endif // ASYNC_NET_SSL_WOLFSSL
