#pragma once

#include "../io/io_context.hpp"
#include "../net/dtls.hpp"
#include "../net/ssl.hpp"
#include "peer.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace async_net::p2p {

/// P2P connection with UDP hole punching + DTLS encryption.
///
/// Usage (active side — connect_to):
///   peer_connection conn(ctx, dtls_ctx);
///   if (co_await conn.connect_to(remote_peer, local_udp_port)) {
///       co_await conn.send("hello");
///       auto msg = co_await conn.receive();
///   }
///
/// Usage (passive side — accept_from):
///   peer_connection conn(ctx, dtls_ctx);
///   if (co_await conn.accept_from(remote_peer, local_udp_port)) {
///       auto msg = co_await conn.receive();
///       co_await conn.send("world");
///   }
class peer_connection {
public:
    peer_connection(io_context& ctx, ssl::context& dtls_ctx);
    ~peer_connection();

    peer_connection(peer_connection&&) = delete;
    peer_connection& operator=(peer_connection&&) = delete;
    peer_connection(const peer_connection&) = delete;
    peer_connection& operator=(const peer_connection&) = delete;

    /// Actively connect to a remote peer via UDP hole punching + DTLS.
    /// local_udp_port: the port this peer's UDP socket is bound to
    ///                  (same port registered with tracker).
    Task<bool> connect_to(const peer_info& remote, uint16_t local_udp_port);

    /// Passively accept a connection from a remote peer.
    /// Waits for the remote peer's hole-punch probes, then completes DTLS.
    Task<bool> accept_from(const peer_info& remote, uint16_t local_udp_port);

    /// Send a text message to the connected peer.
    Task<bool> send(const std::string& msg);

    /// Send raw data to the connected peer.
    Task<bool> send(const uint8_t* data, size_t len);

    /// Receive a message from the connected peer.
    /// Blocks (as coroutine) until a message arrives or connection is closed.
    Task<std::optional<peer_message>> receive();

    /// Close the connection.
    void close();

    /// Check if the connection is established.
    bool is_connected() const { return connected_; }

    /// Get remote peer info.
    const peer_info& remote_peer() const { return remote_; }

private:
    /// Perform DTLS handshake on a non-blocking socket.
    Task<bool> do_dtls_handshake(int udp_fd, bool is_server,
                                 const std::string& remote_ip, uint16_t remote_port);

    io_context* ctx_;
    ssl::context& dtls_ctx_;
    peer_info remote_;
    std::unique_ptr<net::dtls_stream> dtls_;
    int socket_fd_ = -1;
    bool connected_ = false;
};

} // namespace async_net::p2p
