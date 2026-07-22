#pragma once

#include "../io/io_context.hpp"
#include "../io/tcp.hpp"
#include "peer.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace async_net::p2p {

/// P2P Tracker Client — connects to a tracker server for peer discovery.
///
/// Usage:
///   tracker_client client(ctx);
///   if (co_await client.connect("127.0.0.1", 9000, "alice", 8001)) {
///       auto peers = co_await client.list_peers();
///       // ... connect to peers via UDP hole punching
///       co_await client.ping();  // keepalive
///       co_await client.disconnect();
///   }
class tracker_client {
public:
    explicit tracker_client(io_context& ctx);

    /// Connect to tracker and register this peer.
    /// Returns true on success. On success, public_endpoint() returns
    /// the address observed by the tracker.
    Task<bool> connect(const char* tracker_host, uint16_t tracker_port,
                       const std::string& peer_id, uint16_t local_udp_port);

    /// Get list of online peers (excluding self).
    Task<std::vector<peer_info>> list_peers();

    /// Send heartbeat to tracker (keeps registration alive).
    Task<bool> ping();

    /// Disconnect from tracker.
    Task<void> disconnect();

    /// Get the public endpoint reported by the tracker (ip:port).
    const std::string& public_endpoint() const { return public_endpoint_; }

    /// Check if connected to tracker.
    bool is_connected() const { return connected_; }

private:
    Task<std::string> read_line();
    Task<bool> write_line(const std::string& line);

    io_context* ctx_;
    tcp::socket sock_;
    std::string my_id_;
    std::string public_endpoint_;
    bool connected_ = false;
};

} // namespace async_net::p2p
