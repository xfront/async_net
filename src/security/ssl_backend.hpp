#pragma once

// Internal SSL backend interface.
// Each SSL backend (wolfSSL / AWS-LC / LibreSSL) provides an implementation.
// This file is NOT part of the public API.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

namespace async_net::ssl::backend {

// Error code constants (backend maps to these)
constexpr int ERR_NONE        = 0;
constexpr int ERR_WANT_READ   = 1;
constexpr int ERR_WANT_WRITE  = 2;
constexpr int ERR_ZERO_RETURN = 3;

/// Initialize the SSL library (idempotent).
void init();

/// Drain and log pending SSL errors.
void drain_errors(const char* prefix);

// --- Context operations ---

/// Create a new SSL context for the given method string.
/// Methods: "tls_server", "tls_client", "tls",
///          "dtls_server", "dtls_client", "dtls", "dtls_peer"
void* ctx_new(const char* method);

/// Free an SSL context.
void ctx_free(void* ctx);

/// Load certificate chain from PEM file. Returns true on success.
bool ctx_use_cert(void* ctx, const char* path);

/// Load private key from PEM file. Returns true on success.
bool ctx_use_key(void* ctx, const char* path);

/// Load CA certificates for peer verification. Returns true on success.
bool ctx_load_verify(void* ctx, const char* path);

/// Set cipher list.
void ctx_set_cipher_list(void* ctx, const char* ciphers);

/// Set peer verification mode.
void ctx_set_verify(void* ctx, bool verify);

/// Set ALPN protocols (wire format: length-prefixed strings).
void ctx_set_alpn_protos(void* ctx, const unsigned char* wire, unsigned int len);

/// Set ALPN select callback (server side).
/// The callback receives client ALPN list and returns selected protocol.
void ctx_set_alpn_select_cb(void* ctx,
    std::function<std::string(const std::vector<std::string>&)>* user_cb);

// --- Stream operations ---

/// Create a new SSL stream from context and socket fd.
void* stream_new(void* ctx, int fd);

/// Free an SSL stream.
void stream_free(void* ssl);

/// Set server mode.
void stream_set_accept_state(void* ssl);

/// Set client mode.
void stream_set_connect_state(void* ssl);

/// Perform one step of SSL handshake. Returns raw SSL result.
int stream_do_handshake(void* ssl);

/// Read data. Returns raw SSL result.
int stream_read(void* ssl, void* buf, int len);

/// Write data. Returns raw SSL result.
int stream_write(void* ssl, const void* buf, int len);

/// Shutdown SSL. Returns raw SSL result.
int stream_shutdown(void* ssl);

/// Get error code for the last operation result.
/// Returns one of ERR_NONE, ERR_WANT_READ, ERR_WANT_WRITE, ERR_ZERO_RETURN.
int stream_get_error(void* ssl, int ret);

/// Get the selected ALPN protocol after handshake.
std::string stream_alpn_selected(void* ssl);

} // namespace async_net::ssl::backend


