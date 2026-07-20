/// P2P Chat Room — peer-to-peer chat with UDP hole punching + DTLS encryption.
///
/// Architecture:
///   1. Connect to tracker, register self
///   2. Periodically query peer list
///   3. For each new peer: UDP hole punching + DTLS handshake
///   4. Read stdin -> broadcast to all connected peers
///   5. Receive messages -> print to stdout
///
/// Usage: ./p2p_chat <tracker_host> <tracker_port> <peer_id> [udp_port] [cert] [key]
///   tracker_host:  tracker server address  (default 127.0.0.1)
///   tracker_port:  tracker server port     (default 9000)
///   peer_id:       this peer's unique name (required)
///   udp_port:      local UDP port          (default 8000)
///   cert:          certificate PEM         (default examples/server_cert.pem)
///   key:           private key PEM         (default examples/server_key.pem)
///
/// Generate test certs:
///   openssl req -x509 -newkey rsa:2048 -keyout examples/server_key.pem \
///     -out examples/server_cert.pem -days 365 -nodes \
///     -subj "/CN=p2p-test"

#ifdef ASYNC_NET_HAS_SSL

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/ssl.hpp>
#include <async_net/p2p/tracker_client.hpp>
#include <async_net/p2p/peer_connection.hpp>
#include <async_net/executor/schedule.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <unistd.h>

using namespace async_net;

static std::atomic<bool> g_running{true};

// Thread-safe message queue for stdin input
static std::mutex g_input_mutex;
static std::queue<std::string> g_input_queue;

// Connected peers: peer_id -> peer_connection
struct PeerState {
    std::unique_ptr<p2p::peer_connection> conn;
    bool connecting = false;
};
static std::map<std::string, PeerState> g_peers;
static std::mutex g_peers_mutex;

// Active tasks (kept alive until completion)
static std::set<Task<void>*> g_active_tasks;
static std::mutex g_tasks_mutex;

// Background thread: read stdin lines into queue
static void stdin_reader() {
    std::string line;
    while (g_running.load()) {
        if (!std::getline(std::cin, line)) {
            g_running.store(false);
            break;
        }
        if (!line.empty()) {
            std::lock_guard lock(g_input_mutex);
            g_input_queue.push(std::move(line));
        }
    }
}

// Task: receive messages from a peer
Task<void> receive_loop(const std::string& peer_id, p2p::peer_connection& conn) {
    while (conn.is_connected() && g_running.load()) {
        auto msg = co_await conn.receive();
        if (!msg.has_value()) {
            std::fprintf(stderr, "[p2p] peer '%s' disconnected\n", peer_id.c_str());
            break;
        }

        if (msg->type == p2p::msg_type::data) {
            std::string text(msg->payload.begin(), msg->payload.end());
            std::printf("[%s] %s\n", peer_id.c_str(), text.c_str());
            std::fflush(stdout);
        }
    }
}

// Task: broadcast a message to all connected peers
Task<void> broadcast(const std::string& msg) {
    std::vector<std::pair<std::string, p2p::peer_connection*>> peers_copy;
    {
        std::lock_guard lock(g_peers_mutex);
        for (auto& [id, state] : g_peers) {
            if (state.conn && state.conn->is_connected()) {
                peers_copy.emplace_back(id, state.conn.get());
            }
        }
    }
    for (auto& [id, conn] : peers_copy) {
        co_await conn->send(msg);
    }
}

// Task: connect to a new peer via DTLS
Task<void> connect_to_peer(io_context& ctx, ssl::context& dtls_client_ctx, ssl::context& dtls_server_ctx,
                           const p2p::peer_info& peer, uint16_t local_port,
                           bool is_initiator) {
    std::string peer_id = peer.id;

    {
        std::lock_guard lock(g_peers_mutex);
        if (g_peers.count(peer_id)) {
            co_return;  // Already connected or connecting
        }
        g_peers[peer_id].connecting = true;
    }

    std::fprintf(stderr, "[p2p] %s peer '%s' @ %s:%d\n",
                 is_initiator ? "connecting to" : "waiting for",
                 peer_id.c_str(), peer.public_address.c_str(), peer.udp_port);

    // Use client context for initiator (DTLS client), server context for responder (DTLS server)
    auto& dtls_ctx = is_initiator ? dtls_client_ctx : dtls_server_ctx;
    auto conn = std::make_unique<p2p::peer_connection>(ctx, dtls_ctx);

    bool ok = false;
    if (is_initiator) {
        ok = co_await conn->connect_to(peer, local_port);
    } else {
        ok = co_await conn->accept_from(peer, local_port);
    }

    if (!ok) {
        std::fprintf(stderr, "[p2p] failed to connect to peer '%s'\n", peer_id.c_str());
        std::lock_guard lock(g_peers_mutex);
        g_peers.erase(peer_id);
        co_return;
    }

    // Store connection and start receive loop
    auto* conn_ptr = conn.get();
    {
        std::lock_guard lock(g_peers_mutex);
        g_peers[peer_id].conn = std::move(conn);
        g_peers[peer_id].connecting = false;
    }

    std::fprintf(stderr, "[p2p] connected to peer '%s'\n", peer_id.c_str());

    // Run receive loop (await it to keep connection alive)
    co_await receive_loop(peer_id, *conn_ptr);

    // Cleanup after disconnect
    {
        std::lock_guard lock(g_peers_mutex);
        g_peers.erase(peer_id);
    }
}

int main(int argc, char* argv[]) {
    const char* tracker_host = "127.0.0.1";
    uint16_t tracker_port = 9000;
    std::string peer_id;
    uint16_t udp_port = 8000;
    const char* cert_file = "examples/server_cert.pem";
    const char* key_file = "examples/server_key.pem";

    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <tracker_host> <tracker_port> <peer_id> [udp_port] [cert] [key]\n",
                     argv[0]);
        return 1;
    }

    tracker_host = argv[1];
    tracker_port = static_cast<uint16_t>(std::atoi(argv[2]));
    peer_id = argv[3];
    if (argc > 4) udp_port = static_cast<uint16_t>(std::atoi(argv[4]));
    if (argc > 5) cert_file = argv[5];
    if (argc > 6) key_file = argv[6];

    std::fprintf(stderr, "=== P2P Chat ===\n");
    std::fprintf(stderr, "Peer ID: %s\n", peer_id.c_str());
    std::fprintf(stderr, "Tracker: %s:%d\n", tracker_host, tracker_port);
    std::fprintf(stderr, "UDP port: %d\n", udp_port);
    std::fprintf(stderr, "Type a message and press Enter to broadcast.\n\n");

    io_context ctx;

    // Setup DTLS contexts — need separate contexts for client and server roles
    // wolfSSL requires matching context type and handshake role
    ssl::context dtls_client_ctx("dtls_client");
    ssl::context dtls_server_ctx("dtls_server");
    for (auto* c : {&dtls_client_ctx, &dtls_server_ctx}) {
        if (!c->use_certificate_file(cert_file)) {
            std::fprintf(stderr, "Failed to load certificate: %s\n", cert_file);
            return 1;
        }
        if (!c->use_private_key_file(key_file)) {
            std::fprintf(stderr, "Failed to load private key: %s\n", key_file);
            return 1;
        }
        c->set_verify_peer(false);  // Don't verify peer cert (self-signed)
    }

    auto guard = ctx.make_work();

    // Start stdin reader thread
    std::thread stdin_thread(stdin_reader);

    // Main chat coroutine
    auto chat_task = [&]() -> Task<void> {
        // Connect to tracker
        p2p::tracker_client tracker(ctx);
        if (!co_await tracker.connect(tracker_host, tracker_port, peer_id, udp_port)) {
            std::fprintf(stderr, "[chat] failed to connect to tracker\n");
            g_running.store(false);
            co_return;
        }

        std::fprintf(stderr, "[chat] registered with tracker, public: %s\n",
                     tracker.public_endpoint().c_str());

        // Track which peers we've already tried to connect to
        std::set<std::string> known_peers;

        while (g_running.load()) {
            // 1. Send heartbeat to tracker
            co_await tracker.ping();

            // 2. Poll for new peers
            auto peers = co_await tracker.list_peers();
            for (const auto& peer : peers) {
                if (peer.id == peer_id) continue;
                if (known_peers.count(peer.id)) continue;

                known_peers.insert(peer.id);

                // Determine who initiates: peer with "smaller" ID initiates
                bool is_initiator = (peer_id < peer.id);

                // Start connection in a heap-allocated coroutine
                auto* task = new Task<void>(connect_to_peer(ctx, dtls_client_ctx, dtls_server_ctx, peer, udp_port, is_initiator));
                {
                    std::lock_guard lock(g_tasks_mutex);
                    g_active_tasks.insert(task);
                }
                task->resume();
            }

            // 3. Process input messages
            {
                std::lock_guard lock(g_input_mutex);
                while (!g_input_queue.empty()) {
                    auto msg = std::move(g_input_queue.front());
                    g_input_queue.pop();
                    auto* task = new Task<void>(broadcast(msg));
                    {
                        std::lock_guard lock(g_tasks_mutex);
                        g_active_tasks.insert(task);
                    }
                    task->resume();
                }
            }

            // 4. Sleep before next poll
            co_await sleep_for(std::chrono::seconds(2), ctx);
        }

        co_await tracker.disconnect();
    }();
    chat_task.resume();

    ctx.run();

    g_running.store(false);
    if (stdin_thread.joinable()) {
        // Can't really join since it's blocking on cin. Detach it.
        stdin_thread.detach();
    }

    guard.reset();
    return 0;
}

#else
#include <cstdio>
int main() {
    std::fprintf(stderr, "P2P chat requires SSL support (compile with ASYNC_NET_WITH_SSL=ON)\n");
    return 1;
}
#endif
