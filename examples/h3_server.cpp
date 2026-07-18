// HTTP/3 Server Example — QUIC/UDP with wolfSSL + ngtcp2 + self-contained H3
#include <async_net/http/http3_session.hpp>
#include <async_net/net/udp.hpp>
#include <async_net/io/io_context.hpp>
#include <iostream>
#include <signal.h>

using namespace async_net;

static bool running = true;
static void sig_handler(int) { running = false; }

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t port = 4433;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    std::string cert_file = "examples/server_cert.pem";
    std::string key_file = "examples/server_key.pem";
    if (argc > 2) cert_file = argv[2];
    if (argc > 3) key_file = argv[3];

    io_context ctx;

    // Create UDP socket
    udp::socket sock(ctx);
    udp::endpoint ep(port);
    if (!sock.bind(ep)) {
        std::cerr << "Failed to bind UDP port " << port << std::endl;
        return 1;
    }

    std::cout << "[H3] Listening on UDP port " << port << std::endl;

    // HTTP/3 server config
    http::http3_session::config cfg;
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    cfg.max_streams = 100;

    std::unique_ptr<http::http3_session> session;
    char buf[4096];
    udp::endpoint from;

    // Local address for QUIC path validation
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::cout << "[H3] Waiting for QUIC packets..." << std::endl;

    while (running) {
        auto n = sock.receive_from(mutable_buffer(buf, sizeof(buf)), from);
        if (n <= 0) continue;

        // Create session on first packet
        if (!session) {
            session = std::make_unique<http::http3_session>(
                http::http3_session::mode::server, cfg);

            session->set_request_handler([](const http::request& req) -> http::response {
                std::cout << "[H3] " << http::to_string(req.method)
                          << " " << req.path << std::endl;
                return http::response_ok("Hello from HTTP/3!");
            });

            // Initialize server QUIC connection from first packet
            if (!session->init_server_from_packet(
                    reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                    reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                    from.sockaddr_ptr(), from.size())) {
                std::cerr << "[H3] init_server_from_packet failed" << std::endl;
                session.reset();
                continue;
            }
        } else {
            // Feed subsequent packets
            session->feed_packet(
                reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                from.sockaddr_ptr(), from.size());
        }

        // Send any output packets as individual datagrams
        auto pkts = session->get_pending_packets();
        for (auto& pkt : pkts) {
            if (!pkt.empty()) {
                sock.send_to(const_buffer(pkt.data(), pkt.size()), from);
            }
        }

        // Handle QUIC timers
        session->handle_expiry();
    }

    std::cout << "[H3] Shutting down" << std::endl;
    sock.close();
    return 0;
}
