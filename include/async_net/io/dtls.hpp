#pragma once

#include "../detail/config.hpp"
#include "ssl.hpp"
#include <cstddef>
#include <cstdint>
#ifndef ASYNC_NET_WINDOWS
#include <sys/socket.h>
#endif

namespace async_net::net {

namespace dtls_backend { struct dtls_handle; }

/// Non-blocking DTLS stream over a raw UDP file descriptor.
/// Uses backend-specific I/O mechanisms (wolfSSL custom callbacks or AWS-LC BIO)
/// to properly handle EAGAIN, enabling fully async operation without threads.
class dtls_stream {
public:
    /// Construct a DTLS stream on a non-blocking UDP file descriptor.
    dtls_stream(int fd, ssl::context& ctx, bool is_server);
    ~dtls_stream();

    dtls_stream(dtls_stream&& other) noexcept;
    dtls_stream& operator=(dtls_stream&& other) noexcept;

    dtls_stream(const dtls_stream&) = delete;
    dtls_stream& operator=(const dtls_stream&) = delete;

    /// Begin DTLS handshake (sets connect/accept state).
    void begin_handshake();

    /// Perform one step of DTLS handshake.
    /// Returns 0 on success, WANT_READ/WANT_WRITE if needs I/O, other on error.
    int handshake_step();

    /// Blocking handshake with internal retry (for simple/multicast usage).
    /// Returns 0 on success, -1 on failure.
    int handshake();

    /// For server: detect the peer address from the next incoming UDP packet.
    int set_peer_from_socket();

    /// Non-blocking read. Returns bytes read, 0 if would block, -1 on error.
    int read(void* buf, size_t len);

    /// Non-blocking write. Returns bytes written, 0 if would block, -1 on error.
    int write(const void* buf, size_t len);

    /// Send DTLS shutdown alert.
    void shutdown();

    /// Explicitly set the DTLS peer address (for client).
    int set_peer(const char* ip, uint16_t port);

    /// Get the underlying file descriptor.
    int fd() const { return fd_; }

    /// Check if the last operation wants read.
    bool wants_read() const;

    /// Check if the last operation wants write.
    bool wants_write() const;

private:
    int fd_ = -1;
    bool is_server_ = false;
    dtls_backend::dtls_handle* handle_ = nullptr;
};

} // namespace async_net::net
