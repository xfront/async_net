#pragma once

#include "../io/io_context.hpp"
#include "../net/tcp.hpp"
#include "peer.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace async_net {
namespace p2p {

/// P2P Tracker Server — central rendezvous point for peer discovery.
///
/// Peers connect via TCP and use a simple text-line protocol:
///   REGISTER <peer_id> <udp_port> [<tcp_port>]\n
///   LIST\n
///   PING <peer_id>\n
///   QUIT\n
///
/// Server responses:
///   OK <public_ip>:<port>\n
///   PEERS <count>\n<id> <ip> <udp_port>\n...
///   PONG\n
///   ERROR <message>\n
class tracker_server {
public:
    tracker_server(io_context& ctx, uint16_t port);

    /// Run the tracker server (coroutine). Accepts connections and handles peers.
    Task<void> run();

    /// Get current online peer list.
    std::vector<peer_info> peers() const;

    /// Set peer timeout (peers not seen within this duration are removed).
    void set_timeout(std::chrono::seconds t);

    /// Stop the server.
    void stop();

private:
    Task<void> handle_client(tcp::socket sock);
    void cleanup_stale_peers();

    // Read a line from the socket (up to \n). Returns empty on error.
    Task<std::string> read_line(tcp::socket& sock);

    // Write a string to the socket.
    Task<bool> write_line(tcp::socket& sock, const std::string& line);

    io_context* ctx_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    bool running_ = false;

    // Active client tasks (kept alive until completion)
    std::set<Task<void>*> active_tasks_;
    std::mutex tasks_mutex_;

    // Registered peers: peer_id -> peer_info
    mutable std::mutex peers_mutex_;
    std::map<std::string, peer_info> peers_;

    // Connection -> peer_id mapping (for cleanup on disconnect)
    std::map<int, std::string> connection_peers_;
    std::mutex conn_mutex_;

    std::chrono::seconds peer_timeout_{60};
};

} // namespace p2p
} // namespace async_net
