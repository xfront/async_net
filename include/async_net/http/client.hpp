#pragma once

#include <async_net/http/types.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <string>
#include <optional>
#include <vector>
#include <map>
#include <memory>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/io/ssl.hpp>
#include <async_net/http/http2_session.hpp>
#endif

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#include <async_net/io/udp.hpp>
#endif

namespace async_net::http {

// ---------------------------------------------------------------------------
// HTTP Client — send requests, receive responses, connection reuse
// ---------------------------------------------------------------------------

class client {
public:
    explicit client(io_context& ctx);

#ifdef ASYNC_NET_HAS_SSL
    client(io_context& ctx, ssl::context& ssl_ctx);
#endif

    ~client();

    // Convenience methods
    Task<response> get(const std::string& url);
    Task<response> post(const std::string& url, std::string body,
                        const std::string& content_type = "text/plain");
    Task<response> put(const std::string& url, std::string body,
                       const std::string& content_type = "text/plain");
    Task<response> delete_(const std::string& url);

    // General: send any request
    Task<response> send(request req);

    // Send request to specific host
    Task<response> send(const std::string& host, uint16_t port, request req, bool use_tls = false);

    // Close all idle connections in the pool
    void close_idle_connections();

private:
    io_context& ctx_;
#ifdef ASYNC_NET_HAS_SSL
    ssl::context* ssl_ctx_ = nullptr;
#endif

    // Connection pool key: "host:port:tls"
    static std::string pool_key(const std::string& host, uint16_t port, bool tls);

    // H1 idle connection (socket + optional SSL stream, heap-allocated)
    struct h1_conn {
        std::unique_ptr<tcp::socket> sock;
#ifdef ASYNC_NET_HAS_SSL
        std::unique_ptr<ssl::stream> ssl_stream;
#endif
        bool is_tls = false;
    };

    // Pool of idle H1 connections
    std::map<std::string, std::vector<std::unique_ptr<h1_conn>>> h1_pool_;

#ifdef ASYNC_NET_HAS_SSL
    // Pool of idle H2 sessions (multiplexed connections)
    struct h2_conn {
        std::unique_ptr<tcp::socket> sock;
        std::unique_ptr<ssl::stream> ssl_stream;
        std::unique_ptr<http2_session> session;
    };
    std::map<std::string, std::vector<std::unique_ptr<h2_conn>>> h2_pool_;
#endif

#ifdef ASYNC_NET_HAS_HTTP3
    // Pool of idle H3 sessions (QUIC/UDP connections)
    struct h3_conn {
        std::unique_ptr<udp::socket> sock;
        std::unique_ptr<http3_session> session;
        udp::endpoint remote_ep;
    };
    std::map<std::string, std::vector<std::unique_ptr<h3_conn>>> h3_pool_;

    // Send request via HTTP/3 (QUIC/UDP)
    Task<response> send_h3(const std::string& host, uint16_t port, request req);
#endif

    // Return an H1 connection to the pool
    void return_h1(const std::string& key, std::unique_ptr<h1_conn> conn);

    // Take an idle H1 connection from the pool (returns nullptr if none)
    std::unique_ptr<h1_conn> take_h1(const std::string& key);

    // Read a complete HTTP response from socket
    Task<std::optional<response>> read_response(tcp::socket& sock);

#ifdef ASYNC_NET_HAS_SSL
    Task<std::optional<response>> read_response_ssl(ssl::stream& stream);
#endif
};

} // namespace async_net::http
