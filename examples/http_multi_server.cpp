// Multi-Protocol HTTP Server — unified HTTPS URL serving HTTP/1.1, HTTP/2, and HTTP/3
//
// Architecture (same as Cloudflare, Google):
//   TCP port 8443  → HTTP/1.1 + HTTP/2 (TLS + ALPN negotiation)
//   UDP port 8443  → HTTP/3 (QUIC) — same port number, different transport
//
// A single URL (https://localhost:8443/) serves all three protocols:
//   - Client connects via TCP+TLS → ALPN selects h2 or http/1.1
//   - Server advertises H3 via Alt-Svc response header
//   - Client discovers H3 endpoint and upgrades to QUIC on next request
//
// Usage: ./http_multi_server [tls_port] [h3_port] [cert.pem] [key.pem]
//
// Test:
//   curl -k https://localhost:8443/                  # HTTP/1.1 or H2 (ALPN)
//   curl --http2 -k https://localhost:8443/          # Force HTTP/2
//   curl --http3 -k https://localhost:8443/           # HTTP/3 (needs curl with H3)

#include <async_net/http/server.hpp>
#include <async_net/net/udp.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#endif

#include <iostream>
#include <signal.h>
#include <memory>
#include <vector>

using namespace async_net;
using namespace async_net::http;

static bool g_running = true;
static void sig_handler(int) { g_running = false; }

// H3 port for Alt-Svc header (set at startup, same as TLS port by default)
static uint16_t g_h3_port = 8443;

// ---------------------------------------------------------------------------
// Wrap a handler to inject Alt-Svc header for H3 discovery
// ---------------------------------------------------------------------------

static auto with_alt_svc(handler_fn handler) -> handler_fn {
    return [h = std::move(handler)](const request& req) -> Task<response> {
        response resp = co_await h(req);
        // Advertise HTTP/3 endpoint via Alt-Svc
        resp.hdrs.set("Alt-Svc", "h3=\":" + std::to_string(g_h3_port) + "\"; ma=86400");
        co_return resp;
    };
}

// ---------------------------------------------------------------------------
// Route setup — shared across TLS (H1+H2) and H3
// ---------------------------------------------------------------------------

static void setup_routes(server& srv) {
    auto alt_svc = [](handler_fn h) { return with_alt_svc(std::move(h)); };

    // GET /
    srv.route(method::GET, "/", alt_svc([](const request& req) -> Task<response> {
        std::string proto = "HTTP/1.1";
        if (req.ver == version::HTTP_2) proto = "HTTP/2";
        else if (req.ver == version::HTTP_3) proto = "HTTP/3";

        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "text/html")
            .body("<h1>Hello from async_net!</h1>"
                  "<p>Protocol: <b>" + proto + "</b></p>"
                  "<p>Method: " + std::string(to_string(req.method)) + "</p>"
                  "<p>Path: " + req.path + "</p>"
                  "<ul>"
                  "<li><a href=\"/json\">/json</a></li>"
                  "<li><a href=\"/headers\">/headers</a></li>"
                  "<li><a href=\"/info\">/info</a></li>"
                  "</ul>")
            .build();
    }));

    // GET /json
    srv.route(method::GET, "/json", alt_svc([](const request& req) -> Task<response> {
        std::string proto = "http1";
        if (req.ver == version::HTTP_2) proto = "http2";
        else if (req.ver == version::HTTP_3) proto = "http3";

        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "application/json")
            .body(R"({"protocol":")" + proto + R"(","status":"ok","server":"async_net"})")
            .build();
    }));

    // GET /headers — echo all request headers
    srv.route(method::GET, "/headers", alt_svc([](const request& req) -> Task<response> {
        std::string body_text = "Request Headers:\n";
        for (auto& [k, v] : req.hdrs) {
            body_text += "  " + k + ": " + v + "\n";
        }
        co_return response_ok(body_text);
    }));

    // GET /info — server info
    srv.route(method::GET, "/info", alt_svc([](const request& req) -> Task<response> {
        std::string proto = "HTTP/1.1";
        if (req.ver == version::HTTP_2) proto = "HTTP/2";
        else if (req.ver == version::HTTP_3) proto = "HTTP/3";

        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "text/plain")
            .body("Server: async_net multi-protocol\n"
                  "Protocol: " + proto + "\n"
                  "Method: " + std::string(to_string(req.method)) + "\n"
                  "Path: " + req.path + "\n")
            .build();
    }));

    // POST /echo — echo back request body
    srv.route(method::POST, "/echo", alt_svc([](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", req.hdrs.get("Content-Type").value_or("text/plain"))
            .body(req.bd.data())
            .build();
    }));

    // Default: 404
    srv.default_handler([](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::not_found())
            .body("404 Not Found: " + req.path)
            .build();
    });
}

// ---------------------------------------------------------------------------
// HTTP/3 QUIC listener (async UDP task)
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_HTTP3
static Task<void> serve_h3(io_context& ctx, uint16_t port,
                           std::string cert_file, std::string key_file) {
    udp::socket sock(ctx);
    udp::endpoint ep(port);
    if (!sock.bind(ep)) {
        std::cerr << "[H3] Failed to bind UDP port " << port << std::endl;
        co_return;
    }

    std::cout << "[H3] Listening on UDP port " << port << std::endl;

    http::http3_session::config cfg;
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    cfg.max_streams = 100;

    std::unique_ptr<http::http3_session> session;
    char buf[4096];
    udp::endpoint from;

    // Local address for QUIC path validation (must match between init and feed_packet)
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    udp::endpoint last_from;

    while (g_running) {
        auto n = co_await sock.async_receive_from(mutable_buffer(buf, sizeof(buf)), from);
        if (n <= 0) continue;

        // Reset session if a different client connects
        if (session && (from.port() != last_from.port() ||
                        from.address() != last_from.address())) {
            session.reset();
        }

        if (!session) {
            session = std::make_unique<http::http3_session>(
                http::http3_session::mode::server, cfg);

            session->set_request_handler([](const request& req) -> response {
                std::cout << "[H3] " << to_string(req.method) << " " << req.path << std::endl;

                if (req.path == "/" || req.path.empty()) {
                    return response_make()
                        .status(status_code::ok())
                        .header("Content-Type", "text/html")
                        .body("<h1>Hello from HTTP/3!</h1>"
                              "<p>Protocol: <b>HTTP/3</b></p>")
                        .build();
                }
                if (req.path == "/json") {
                    return response_make()
                        .status(status_code::ok())
                        .header("Content-Type", "application/json")
                        .body(R"({"protocol":"http3","status":"ok","server":"async_net"})")
                        .build();
                }
                if (req.path == "/echo" && req.method == method::POST) {
                    return response_make()
                        .status(status_code::ok())
                        .header("Content-Type", "text/plain")
                        .body(req.bd.data())
                        .build();
                }
                return response_make()
                    .status(status_code::not_found())
                    .body("404 Not Found: " + req.path)
                    .build();
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
        }

        session->feed_packet(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                              reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                              from.sockaddr_ptr(), from.size());

        auto pkts = session->get_pending_packets();
        for (auto& pkt : pkts) {
            if (!pkt.empty()) {
                sock.send_to(const_buffer(pkt.data(), pkt.size()), from);
            }
        }

        session->handle_expiry();
        last_from = from;
    }

    sock.close();
}
#endif

// ---------------------------------------------------------------------------
// Main — unified HTTPS + H3
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t tls_port = 8443;
    uint16_t h3_port = 8443;
    const char* cert = "examples/server_cert.pem";
    const char* key = "examples/server_key.pem";

    if (argc > 1) tls_port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) h3_port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) cert = argv[3];
    if (argc > 4) key = argv[4];

    g_h3_port = h3_port;

    try {
        io_context ctx;
        std::cout << "Backend: " << ctx.backend().name() << std::endl;

        // Keep serve tasks alive for the entire server lifetime
        std::vector<std::unique_ptr<Task<void>>> tasks;

#ifdef ASYNC_NET_HAS_SSL
        // --- HTTP/1.1 + HTTP/2 (TLS + ALPN on single port) ---
        server srv(ctx, tls_port);
        setup_routes(srv);

        ssl::context ssl_ctx("tls_server");
        if (!ssl_ctx.use_certificate_file(cert)) {
            std::cerr << "Failed to load certificate: " << cert << std::endl;
            return 1;
        }
        if (!ssl_ctx.use_private_key_file(key)) {
            std::cerr << "Failed to load key: " << key << std::endl;
            return 1;
        }

        std::cout << "[TLS] Starting HTTP/1.1+H2 on port " << tls_port << std::endl;
        {
            auto t = std::make_unique<Task<void>>(srv.serve_h2(ssl_ctx));
            t->resume();  // Start TLS accept loop
            tasks.push_back(std::move(t));
        }
#else
        std::cerr << "This server requires SSL support (not compiled in)" << std::endl;
        return 1;
#endif

#ifdef ASYNC_NET_HAS_HTTP3
        // --- HTTP/3 (QUIC/UDP) ---
        std::cout << "[H3] Starting HTTP/3 on UDP port " << h3_port << std::endl;
        {
            auto t = std::make_unique<Task<void>>(serve_h3(ctx, h3_port, cert, key));
            t->resume();  // Start UDP receive loop
            tasks.push_back(std::move(t));
        }
#endif

        std::cout << "\n=== Multi-Protocol HTTP Server ===" << std::endl;
#ifdef ASYNC_NET_HAS_SSL
        std::cout << "  https://localhost:" << tls_port
                  << "/  (HTTP/1.1 + HTTP/2 via ALPN)" << std::endl;
#endif
#ifdef ASYNC_NET_HAS_HTTP3
        std::cout << "  https://localhost:" << h3_port
                  << "/  (HTTP/3 via QUIC/UDP)" << std::endl;
        std::cout << "  Alt-Svc header advertises H3 on port " << h3_port << std::endl;
#endif
        std::cout << "==================================\n" << std::endl;

        // Run the event loop — blocks until io_context stops
        ctx.run();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Server stopped." << std::endl;
    return 0;
}
