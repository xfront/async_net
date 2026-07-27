#pragma once

// ---------------------------------------------------------------------------
// server_builder — Builder pattern for HTTP server configuration
//
// Provides a fluent API for configuring and launching an HTTP server
// using network::acceptor<service_handler> internally.
//
// Internally creates the correct service handlers:
//   - http1()         → http11_handler (plain HTTP/1.1)
//   - tls(ssl_ctx)    → https_handler   (TLS+ALPN, H2+H1.1)
//
// Usage:
//   server_builder(ctx, 8080)
//       .route(method::GET, "/", my_handler)
//       .route(method::GET, "/json", json_handler)
//       .tls(ssl_ctx)          // Enable TLS + H2 via ALPN
//       .http3(cert, key)      // Enable HTTP/3 on same port (UDP)
//       .run();                // Build + serve (blocks)
//
// Or build without running:
//   auto srv = server_builder(ctx, 8080)
//       .route(method::GET, "/", my_handler)
//       .build();
//   co_await srv.serve_configured();
// ---------------------------------------------------------------------------

#include <async_net/http/server.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/ssl.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace async_net::http {

class server_builder {
public:
    explicit server_builder(io_context& ctx, uint16_t port = 8080)
        : ctx_(&ctx), port_(port) {}

    // --- Network configuration ---

    server_builder& address(const char* addr) { addr_ = addr; return *this; }
    server_builder& reuse_port(bool rp = true) { reuse_port_ = rp; return *this; }

    // --- Route registration ---

    server_builder& route(method m, const std::string& path, handler_fn handler) {
        routes_.push_back({m, path, std::move(handler)});
        return *this;
    }

    server_builder& ws_route(const std::string& path, ws::ws_handler_fn handler) {
        ws_routes_.push_back({path, std::move(handler)});
        return *this;
    }

    server_builder& default_handler(handler_fn handler) {
        default_handler_ = std::move(handler);
        return *this;
    }

    server_builder& push_provider(server::push_provider provider) {
        push_provider_ = std::move(provider);
        return *this;
    }

    // --- Protocol selection ---

    /// Plain HTTP/1.1 (default when no TLS is configured)
    server_builder& http1() { use_tls_ = false; return *this; }

#ifdef ASYNC_NET_HAS_SSL
    /// TLS with ALPN negotiation (HTTP/2 + HTTP/1.1 fallback)
    server_builder& tls(ssl::context& ssl_ctx) {
        use_tls_ = true;
        ssl_ctx_ = &ssl_ctx;
        return *this;
    }
#endif

#ifdef ASYNC_NET_HAS_HTTP3
    /// Enable HTTP/3 (QUIC/UDP) on the same port number
    server_builder& http3(const std::string& cert_file, const std::string& key_file) {
        use_h3_ = true;
        h3_cert_ = cert_file;
        h3_key_ = key_file;
        return *this;
    }
#endif

    // --- Build ---

    /// Build the server with the configured acceptor and routes.
    /// The returned server is ready to serve via serve_configured().
    std::unique_ptr<server> build();

    /// Build + run (blocks until server stops).
    /// mp : multi process
    /// num_workers: 0 = auto-detect hardware concurrency.
    void run(bool mp = false, unsigned num_workers = 0);

private:
    struct route_entry {
        method m;
        std::string path;
        handler_fn handler;
    };

    struct ws_route_entry {
        std::string path;
        ws::ws_handler_fn handler;
    };

    io_context* ctx_;
    uint16_t port_;
    std::string addr_ = "0.0.0.0";
    bool reuse_port_ = false;

    std::vector<route_entry> routes_;
    std::vector<ws_route_entry> ws_routes_;
    handler_fn default_handler_;
    server::push_provider push_provider_;

    // Protocol configuration
    bool use_tls_ = false;
    ssl::context* ssl_ctx_ = nullptr;
    bool use_h3_ = false;
    std::string h3_cert_;
    std::string h3_key_;
};

} // namespace async_net::http
