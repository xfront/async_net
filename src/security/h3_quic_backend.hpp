#pragma once

// Internal HTTP/3 QUIC SSL backend interface.
// Each SSL backend provides an implementation for QUIC TLS 1.3 operations.
// This file is NOT part of the public API.

#include <cstdint>
#include <cstddef>

extern "C" {
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
}

namespace async_net::http::quic_ssl {

// -----------------------------------------------------------------------
// ngtcp2 crypto callbacks (backend provides these, used as ngtcp2 callbacks)
// -----------------------------------------------------------------------

/// AEAD encrypt callback for ngtcp2.
int encrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
               const ngtcp2_crypto_aead_ctx *aead_ctx,
               const uint8_t *plaintext, size_t plaintextlen,
               const uint8_t *nonce, size_t noncelen,
               const uint8_t *aad, size_t aadlen);

/// AEAD decrypt callback for ngtcp2.
int decrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
               const ngtcp2_crypto_aead_ctx *aead_ctx,
               const uint8_t *ciphertext, size_t ciphertextlen,
               const uint8_t *nonce, size_t noncelen,
               const uint8_t *aad, size_t aadlen);

// -----------------------------------------------------------------------
// Opaque SSL handle types — backend manages these
// -----------------------------------------------------------------------

/// Opaque SSL context handle (WOLFSSL_CTX* or SSL_CTX*)
struct ssl_ctx_handle;

/// Opaque SSL object handle (WOLFSSL* or SSL*)
struct ssl_handle;

// -----------------------------------------------------------------------
// Context lifecycle & configuration
// -----------------------------------------------------------------------

/// Create a new TLS 1.3 server context.
ssl_ctx_handle* ctx_new_server();

/// Create a new TLS 1.3 client context.
ssl_ctx_handle* ctx_new_client();

/// Free a context.
void ctx_free(ssl_ctx_handle* ctx);

/// Load certificate chain from PEM file. Returns true on success.
bool ctx_use_cert(ssl_ctx_handle* ctx, const char* cert_file);

/// Load private key from PEM file. Returns true on success.
bool ctx_use_key(ssl_ctx_handle* ctx, const char* key_file);

/// Set ALPN to "h3" on client context.
void ctx_set_alpn_h3_client(ssl_ctx_handle* ctx);

/// Set ALPN select callback for "h3" on server context.
void ctx_set_alpn_select_h3_server(ssl_ctx_handle* ctx);

/// Disable peer certificate verification on client context.
void ctx_set_verify_none(ssl_ctx_handle* ctx);

// -----------------------------------------------------------------------
// SSL object lifecycle & configuration
// -----------------------------------------------------------------------

/// Create a new SSL object from context.
ssl_handle* ssl_new(ssl_ctx_handle* ctx);

/// Free an SSL object.
void ssl_free(ssl_handle* ssl);

/// Set QUIC method and app data (conn_ref) on the SSL object.
/// conn_ref_ptr: pointer to ngtcp2_crypto_conn_ref struct.
void ssl_set_quic(ssl_handle* ssl, void* conn_ref_ptr);

/// Get the native SSL handle (WOLFSSL* or SSL*) for ngtcp2_conn_set_tls_native_handle.
void* ssl_native_handle(ssl_handle* ssl);

} // namespace async_net::http::quic_ssl
