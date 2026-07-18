#pragma once

#include "../detail/config.hpp"
#include "ssl.hpp"
#include <cstddef>
#include <cstdint>

namespace async_net {
namespace net {

/// Blocking DTLS stream over a raw UDP file descriptor.
/// Wraps wolfSSL DTLS operations so that user code never touches wolfSSL directly.
///
/// Server usage:
///   int fd = /* raw blocking UDP socket */;
///   dtls_stream stream(fd, ctx, /*is_server=*/true);
///   stream.set_peer_from_socket();   // peek client addr
///   stream.handshake();
///   stream.write(key, 32);
///
/// Client usage:
///   int fd = /* raw connected UDP socket */;
///   dtls_stream stream(fd, ctx, /*is_server=*/false);
///   stream.handshake();
///   stream.read(buf, 32);
class dtls_stream {
public:
    /// Construct a DTLS stream on an existing UDP file descriptor.
    /// The fd must be a blocking socket suitable for DTLS I/O.
    /// For server: the caller should call set_peer_from_socket() before handshake.
    dtls_stream(int fd, ssl::context& ctx, bool is_server);
    ~dtls_stream();

    dtls_stream(dtls_stream&& other) noexcept;
    dtls_stream& operator=(dtls_stream&& other) noexcept;

    dtls_stream(const dtls_stream&) = delete;
    dtls_stream& operator=(const dtls_stream&) = delete;

    /// Perform DTLS handshake (blocking, with internal retransmission).
    /// Returns 0 on success, -1 on failure.
    int handshake();

    /// Read exactly `len` bytes from the DTLS channel.
    /// Returns number of bytes read, or -1 on error.
    int read(void* buf, size_t len);

    /// Write `len` bytes to the DTLS channel.
    /// Returns number of bytes written, or -1 on error.
    int write(const void* buf, size_t len);

    /// Send DTLS shutdown alert.
    void shutdown();

    /// For server: detect the peer address from the next incoming UDP packet
    /// on the underlying fd and configure the DTLS peer accordingly.
    /// Must be called before handshake() on the server side.
    /// Returns 0 on success, -1 on failure.
    int set_peer_from_socket();

    /// Get the underlying file descriptor.
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    void* ssl_ = nullptr;   // WOLFSSL* (opaque to avoid exposing wolfSSL headers)
    bool is_server_ = false;
};

} // namespace net
} // namespace async_net
