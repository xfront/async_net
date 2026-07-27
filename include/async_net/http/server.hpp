#pragma once

// ---------------------------------------------------------------------------
// HTTP Server — Acceptor/Service Handler architecture
//
// The server holds shared routing state (server_context) and uses
// network::acceptor<service_handler> for passive connection establishment.
//
// Service handlers (http11_handler, https_handler) inherit from
// network::service_handler and implement the connection lifecycle.
// ---------------------------------------------------------------------------

#include <async_net/http/types.hpp>
#include <async_net/http/handler.hpp>
#include <async_net/http/websocket.hpp>
#include <async_net/network/service_handler.hpp>
#include <async_net/network/tls_handler.hpp>
#include <async_net/network/acceptor.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <mutex>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/io/ssl.hpp>
#include <async_net/http/http2_session.hpp>
#endif

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#include <async_net/io/udp.hpp>
#include <async_net/network/connector.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <arpa/inet.h>
#endif
#endif

namespace async_net::http {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class server;
class http11_handler;
class https_handler;

// ---------------------------------------------------------------------------
// server_context — shared routing state for all service handlers
// ---------------------------------------------------------------------------

struct server_context {
    io_context* io_ctx = nullptr;
    uint16_t port = 0;
    std::string addr = "0.0.0.0";
    bool reuse_port = false;
    std::atomic<bool> running{true};

    // Route table
    struct route_entry { method m; std::string path; handler_fn handler; };
    std::vector<route_entry> routes;
    std::vector<std::pair<std::string, ws::ws_handler_fn>> ws_routes;
    handler_fn default_handler;

    // H2/H3 push
    using push_provider = std::function<std::vector<std::pair<request, response>>(const request&)>;
    push_provider push_provider_fn;

    // H3 config
    bool h3_enabled = false;
    std::string h3_cert, h3_key;
    std::unique_ptr<Task<void>> h3_task;  // Keeps H3 coroutine alive

    // Active acceptors (created lazily by serve methods)
    std::unique_ptr<network::acceptor<http11_handler>> h1_acceptor;
#ifdef ASYNC_NET_HAS_SSL
    std::unique_ptr<network::acceptor<https_handler>> tls_acceptor;
#endif
};

// ---------------------------------------------------------------------------
// server — coordinating class
// ---------------------------------------------------------------------------

class server {
public:
    server(io_context& ctx, uint16_t port, const char* addr = "0.0.0.0", bool reuse_port = false);

    // --- Route registration ---
    void route(method m, const std::string& path, handler_fn handler);
    void ws_route(const std::string& path, ws::ws_handler_fn handler);
    void default_handler(handler_fn handler);

    using push_provider = server_context::push_provider;
    void set_push_provider(push_provider provider);

    // --- Serve methods ---

    /// HTTP/1.1 plain text
    Task<void> serve();

#ifdef ASYNC_NET_HAS_SSL
    /// TLS + ALPN (HTTP/2 with HTTP/1.1 fallback)
    Task<void> serve(ssl::context& ssl_ctx);

    /// TLS + HTTP/2 only (ALPN: h2 + http/1.1)
    Task<void> serve_h2(ssl::context& ssl_ctx);
#endif

#ifdef ASYNC_NET_HAS_HTTP3
    /// HTTP/3 over QUIC/UDP
    Task<void> serve_h3(const std::string& cert_file, const std::string& key_file);
#endif

    /// Serve configured protocols (set via builder)
    Task<void> serve_configured();

    // --- Control ---
    void stop();

    // --- Accessors ---
    io_context& get_io_context() { return *ctx_->io_ctx; }
    server_context& context() { return *ctx_; }
    const server_context& context() const { return *ctx_; }

private:
    std::shared_ptr<server_context> ctx_;  // Shared with all handlers

    friend class http11_handler;
    friend class https_handler;
    friend class server_builder;
};

// ============================================================================
// Service Handler classes (defined in server_impl.cpp)
// ============================================================================

// ---------------------------------------------------------------------------
// http11_handler — plain HTTP/1.1 keep-alive handler
// ---------------------------------------------------------------------------

class http11_handler : public network::service_handler<tcp::socket> {
public:
    explicit http11_handler(std::shared_ptr<server_context> ctx)
        : ctx_(std::move(ctx)) {}

    Task<void> run() override;

private:
    std::shared_ptr<server_context> ctx_;
};

// ---------------------------------------------------------------------------
// https_handler — TLS + ALPN handler (HTTP/2 with HTTP/1.1 fallback)
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_SSL
class https_handler : public network::tls_handler {
public:
    https_handler(std::shared_ptr<server_context> ctx, ssl::context& ssl_ctx)
        : tls_handler(ssl_ctx, true), ctx_(std::move(ctx)) {}

    Task<void> run_tls(ssl::stream& strm) override;

private:
    std::shared_ptr<server_context> ctx_;
};
#endif

} // namespace async_net::http
