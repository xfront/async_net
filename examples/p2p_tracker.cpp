/// P2P Tracker Server
///
/// A central rendezvous point for peer discovery.
/// Peers register with the tracker and query the list of online peers.
///
/// Usage: ./p2p_tracker [port]
/// Default port: 9000

#include <algorithm>
#include <async_net/executor/schedule.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/p2p/tracker_server.hpp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace async_net;

static p2p::tracker_server* g_server = nullptr;

static void signal_handler(int) {
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    if (argc > 1) {
        // Support both ./p2p_tracker [port] and ./p2p_tracker [host] [port]
        int arg_idx = 1;
        // If first arg looks like a port (pure number), use it directly
        std::string arg1 = argv[1];
        bool is_number = !arg1.empty() && std::all_of(arg1.begin(), arg1.end(), ::isdigit);
        if (!is_number && argc > 2) {
            arg_idx = 2;  // Skip host, use second arg as port
        }
        port = static_cast<uint16_t>(std::atoi(argv[arg_idx]));
    }

    std::fprintf(stderr, "=== P2P Tracker Server ===\n");
    std::fprintf(stderr, "Listening on port %d\n", port);
    std::fprintf(stderr, "Press Ctrl+C to stop\n\n");

    io_context ctx;

    p2p::tracker_server server(ctx, port);
    g_server = &server;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto guard = ctx.make_work();

    // Start the tracker server
    auto server_task = server.run();
    server_task.resume();

    // Periodic status report
    auto status_task = [&]() -> Task<void> {
        while (true) {
            co_await sleep_for(std::chrono::seconds(30), ctx);
            auto peers = server.peers();
            std::fprintf(stderr, "[tracker] %zu peers online\n", peers.size());
            for (const auto& p : peers) {
                std::fprintf(stderr, "  - %s @ %s:%d\n", p.id.c_str(), p.public_address.c_str(), p.udp_port);
            }
        }
    }();
    status_task.resume();

    ctx.run();

    guard.reset();
    return 0;
}
