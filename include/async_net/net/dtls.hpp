#pragma once

#include "../detail/config.hpp"
#include "ssl.hpp"
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

namespace async_net {
namespace net {

/// Non-blocking DTLS stream over a raw UDP file descriptor.
/// Uses wolfSSL custom I/O callbacks to properly handle EAGAIN,
/// enabling fully async operation without threads.
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
    /// Returns 0 on success, WOLFSSL_ERROR_WANT_READ/WANT_WRITE if needs I/O, other on error.
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
    bool wants_read() const { return last_want_read_; }

    /// Check if the last operation wants write.
    bool wants_write() const { return last_want_write_; }

private:
    // I/O context passed to wolfSSL custom callbacks
    struct io_ctx {
        int fd;
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len;
        bool has_peer_addr;
    };

    static int io_recv_callback(WOLFSSL* ssl, char* buf, int sz, void* ctx);
    static int io_send_callback(WOLFSSL* ssl, char* buf, int sz, void* ctx);

    void track_want(int err);

    int fd_ = -1;
    void* ssl_ = nullptr;   // WOLFSSL*
    io_ctx io_ctx_{};
    bool is_server_ = false;
    bool last_want_read_ = false;
    bool last_want_write_ = false;
};

} // namespace net
} // namespace async_net
