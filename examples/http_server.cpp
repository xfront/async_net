// HTTP Server — unified multi-protocol server (HTTP/1.1 + HTTP/2 + HTTP/3)
//
// Architecture (same as Cloudflare, Google):
//   TCP port  → HTTP/1.1 + HTTP/2 (TLS + ALPN negotiation)
//   UDP port  → HTTP/3 (QUIC) — same port number, different transport
//
// A single URL serves all three protocols:
//   - Client connects via TCP+TLS → ALPN selects h2 or http/1.1
//   - Server advertises H3 via Alt-Svc response header
//   - Client discovers H3 endpoint and upgrades to QUIC on next request
//
// Usage: ./http_server [port] [cert.pem] [key.pem]
//
// Test:
//   curl http://localhost:8080/                     # HTTP/1.1 (plain)
//   curl -k https://localhost:8443/                 # HTTP/1.1 or H2 (ALPN)
//   curl --http2 -k https://localhost:8443/         # Force HTTP/2
//   curl --http3 -k https://localhost:8443/         # HTTP/3 (needs curl with H3)

#include <async_net/http/server.hpp>
#include <async_net/http/server_builder.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>

#include <iostream>
#include <csignal>
#include <memory>
#include <vector>

using namespace async_net;
using namespace async_net::http;

static bool g_running = true;
static void sig_handler(int) { g_running = false; }


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t port = 8080;
    const char* cert = "server_cert.pem";
    const char* key = "server_key.pem";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) cert = argv[2];
    if (argc > 3) key = argv[3];

    try
    {
        io_context ctx;
        std::cout << "Backend: " << ctx.backend().name() << std::endl;

        // =====================================================================
        // Builder pattern — configure and run in one fluent expression
        // =====================================================================
        auto builder = server_builder(ctx, port)
            .route(method::GET, "/", [](const request& req) -> Task<response>
            {
                std::string proto = "HTTP/1.1";
#ifdef ASYNC_NET_HAS_SSL
                if (req.ver == version::HTTP_2) proto = "HTTP/2";
#endif
#ifdef ASYNC_NET_HAS_HTTP3
                if (req.ver == version::HTTP_3) proto = "HTTP/3";
#endif
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
            })
            .route(method::GET, "/json", [](const request& req) -> Task<response>
            {
                std::string proto = "http1";
#ifdef ASYNC_NET_HAS_SSL
                if (req.ver == version::HTTP_2) proto = "http2";
#endif
#ifdef ASYNC_NET_HAS_HTTP3
                if (req.ver == version::HTTP_3) proto = "http3";
#endif
                co_return response_make()
                          .status(status_code::ok())
                          .header("Content-Type", "application/json")
                          .body(R"({"protocol":")" + proto + R"(","status":"ok","server":"async_net"})")
                          .build();
            })
            .route(method::GET, "/headers", [](const request& req) -> Task<response>
            {
                std::string body_text = "Request Headers:\n";
                for (auto& [k, v] : req.hdrs)
                    body_text += "  " + k + ": " + v + "\n";
                co_return response_ok(body_text);
            })
            .route(method::GET, "/info", [](const request& req) -> Task<response>
            {
                std::string proto = "HTTP/1.1";
#ifdef ASYNC_NET_HAS_SSL
                if (req.ver == version::HTTP_2) proto = "HTTP/2";
#endif
#ifdef ASYNC_NET_HAS_HTTP3
                if (req.ver == version::HTTP_3) proto = "HTTP/3";
#endif
                co_return response_make()
                          .status(status_code::ok())
                          .header("Content-Type", "text/plain")
                          .body("Server: async_net\n"
                              "Protocol: " + proto + "\n"
                              "Method: " + std::string(to_string(req.method)) + "\n"
                              "Path: " + req.path + "\n")
                          .build();
            })
            .route(method::POST, "/echo", [](const request& req) -> Task<response>
            {
                co_return response_make()
                          .status(status_code::ok())
                          .header("Content-Type", req.hdrs.get("Content-Type").value_or("text/plain"))
                          .body(req.bd.data())
                          .build();
            })
            .default_handler([](const request& req) -> Task<response>
            {
                co_return response_make()
                          .status(status_code::not_found())
                          .body("404 Not Found: " + req.path)
                          .build();
            });

#ifdef ASYNC_NET_HAS_SSL
        // --- TLS + HTTP/2 (ALPN) ---
        ssl::context ssl_ctx("tls_server");
        if (!ssl_ctx.use_certificate_file(cert))
        {
            std::cerr << "Failed to load certificate: " << cert << std::endl;
            return 1;
        }
        if (!ssl_ctx.use_private_key_file(key))
        {
            std::cerr << "Failed to load key: " << key << std::endl;
            return 1;
        }
        builder.tls(ssl_ctx);
        std::cout << "[http_server] Starting HTTP/1.1+H2 (TLS) on port " << port << std::endl;
#else
        builder.http1();
        std::cout << "[http_server] Starting HTTP/1.1 on port " << port << std::endl;
#endif

#ifdef ASYNC_NET_HAS_HTTP3
        builder.http3(cert, key);
        std::cout << "[http_server] Starting HTTP/3 (QUIC) on UDP port " << port << std::endl;
#endif

        std::cout << "\n=== async_net HTTP Server ===" << std::endl;
#ifdef ASYNC_NET_HAS_SSL
        std::cout << "  https://localhost:" << port << "/  (HTTP/1.1 + HTTP/2 via ALPN)" << std::endl;
#else
        std::cout << "  http://localhost:" << port << "/   (HTTP/1.1)" << std::endl;
#endif
#ifdef ASYNC_NET_HAS_HTTP3
        std::cout << "  https://localhost:" << port << "/  (HTTP/3 via QUIC/UDP)" << std::endl;
#endif
        std::cout << "================================\n" << std::endl;

        // Build + run (blocks until stopped)
        builder.run(false, 2);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Server stopped." << std::endl;
    return 0;
}
