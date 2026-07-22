#pragma once

// Internal DTLS backend interface.
// Each SSL backend provides an implementation.
// This file is NOT part of the public API.

#include <async_net/detail/config.hpp>
#include <cstdint>
#include <cstddef>

namespace async_net::net::dtls_backend {

// Normalized error codes (backend maps from SSL-specific errors)
constexpr int OK             =  0;
constexpr int WANT_READ      =  1;
constexpr int WANT_WRITE     =  2;

/// Opaque handle holding SSL state + I/O context + want flags.
struct dtls_handle;

/// Create a DTLS handle wrapping the SSL object and I/O mechanism.
/// ctx_handle: the ssl::context native handle (WOLFSSL_CTX* or SSL_CTX*).
/// fd: the UDP socket file descriptor.
/// is_server: true for server mode, false for client.
dtls_handle* create(void* ctx_handle, int fd, bool is_server);

/// Destroy a DTLS handle (frees SSL object and I/O context).
void destroy(dtls_handle* h);

/// Move-construct: transfer ownership from other, returns new handle.
/// After this call, other is invalidated (set to nullptr by caller).
dtls_handle* move(dtls_handle* other);

/// Set connect/accept state.
void begin_handshake(dtls_handle* h);

/// Perform one handshake step.
/// Returns OK on success, WANT_READ/WANT_WRITE if needs I/O, ERROR on fatal.
int handshake_step(dtls_handle* h);

/// Blocking handshake with internal retry.
/// Returns OK (0) on success, ERROR (-1) on failure.
int handshake(dtls_handle* h, int fd);

/// Detect peer address from next incoming UDP packet.
int set_peer_from_socket(dtls_handle* h, int fd);

/// Non-blocking read. Returns bytes read, 0 if would block, ERROR on error.
int read(dtls_handle* h, void* buf, size_t len);

/// Non-blocking write. Returns bytes written, 0 if would block, ERROR on error.
int write(dtls_handle* h, const void* buf, size_t len);

/// Send DTLS shutdown alert.
void shutdown(dtls_handle* h);

/// Set peer address explicitly (for client).
int set_peer(dtls_handle* h, const char* ip, uint16_t port);

/// Query want_read flag (set after last operation).
bool wants_read(const dtls_handle* h);

/// Query want_write flag (set after last operation).
bool wants_write(const dtls_handle* h);

} // namespace async_net::net::dtls_backend
