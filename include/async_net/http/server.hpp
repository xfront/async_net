#pragma once

#include <async_net/http/types.hpp>
#include <async_net/http/handler.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <unordered_set>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/net/ssl.hpp>
#endif

#ifdef ASYNC_NET_HAS_HTTP3
#include <string>
#endif

namespace async_net::http {

// ---------------------------------------------------------------------------
// HTTP Server — route-based request handling with keep-alive
// ---------------------------------------------------------------------------

class server {
public:
    server(io_context& ctx, uint16_t port, const char* addr = "0.0.0.0");

    // Register a route handler
    void route(method m, const std::string& path, handler_fn handler);

    // Register a default handler (for unmatched routes, typically returns 404)
    void default_handler(handler_fn handler);

    // Set push provider for H2/H3 server push
    // Returns a list of (promised_request, push_response) pairs to push
    using push_provider = std::function<std::vector<std::pair<request, response>>(const request&)>;
    void set_push_provider(push_provider provider);

    // Start serving (blocks until io_context stops)
    Task<void> serve();

#ifdef ASYNC_NET_HAS_SSL
    // Start serving with TLS
    Task<void> serve_tls(ssl::context& ssl_ctx);

    // Start serving with TLS + HTTP/2 (ALPN: h2 + http/1.1)
    Task<void> serve_h2(ssl::context& ssl_ctx);
#endif

#ifdef ASYNC_NET_HAS_HTTP3
    // Start serving HTTP/3 over QUIC/UDP
    // cert_file/key_file: PEM files for TLS
    Task<void> serve_h3(const std::string& cert_file, const std::string& key_file);
#endif

    // Stop the server
    void stop();

private:
    struct route_entry {
        method m;
        std::string path;
        handler_fn handler;
    };

    io_context& ctx_;
    tcp::acceptor acceptor_;
    uint16_t port_;
    std::vector<route_entry> routes_;
    handler_fn default_handler_;
    push_provider push_provider_;
    bool running_ = true;

    // Active connection tasks (prevent destruction)
    std::unordered_set<Task<void>*> active_tasks_;

    // Handle a single connection (HTTP/1.1 keep-alive loop)
    Task<void> handle_connection(tcp::socket sock);

#ifdef ASYNC_NET_HAS_SSL
    // Handle a single TLS connection
    Task<void> handle_tls_connection(tcp::socket sock, ssl::context& ssl_ctx);

    // Handle a single H2 connection (TLS + ALPN)
    Task<void> handle_h2_connection(tcp::socket sock, ssl::context& ssl_ctx);
#endif

    // Read a complete HTTP request from socket
    Task<std::optional<request>> read_request(tcp::socket& sock);

    // Find and dispatch handler for a request
    Task<response> dispatch(const request& req);

    // Write a response to socket
    Task<bool> write_response(tcp::socket& sock, const response& resp);

    void cleanup_task(Task<void>* task);
};

} // namespace async_net::http
