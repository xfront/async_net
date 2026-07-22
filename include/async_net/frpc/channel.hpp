// FRPC Channel — client-side connection to a FRPC server
// Supports all 4 RPC types over HTTP/2 transport with FlatBuffers serialization.
#pragma once

#include <async_net/frpc/types.hpp>
#include <async_net/http/http2_session.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <functional>
#include <string>
#include <memory>
#include <chrono>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/io/ssl.hpp>
#endif

namespace async_net::frpc {

// ============================================================================
// FRPC Channel
// ============================================================================

class channel {
public:
    // Construct a channel connecting to the given host and port
    channel(io_context& ctx, const std::string& host, uint16_t port);

    // Destructor
    ~channel();

    // Connect to the server (plain HTTP/2 with prior knowledge)
    Task<bool> connect();

#ifdef ASYNC_NET_HAS_SSL
    // Connect with TLS
    Task<bool> connect(ssl::context& ssl_ctx);
#endif

    // Make a unary RPC call
    Task<std::pair<status, std::string>> unary_call(
        const std::string& service,
        const std::string& method,
        const std::string& request_data);

    // Make a unary RPC call with metadata
    Task<std::pair<status, std::string>> unary_call(
        const std::string& service,
        const std::string& method,
        const std::string& request_data,
        const metadata& initial_metadata,
        metadata* response_metadata = nullptr);

    // Server-streaming RPC call
    Task<status> server_stream_call(
        const std::string& service,
        const std::string& method,
        const std::string& request_data,
        std::function<void(const std::string&)> on_message);

    // Client-streaming RPC call
    struct client_stream_state {
        std::shared_ptr<writer> wrt;
        std::shared_ptr<http::http2_session::response_promise> promise;
    };
    Task<client_stream_state> client_stream_call(
        const std::string& service,
        const std::string& method);

    // Bidirectional streaming RPC call
    struct bidi_stream_state {
        std::shared_ptr<reader> rdr;
        std::shared_ptr<writer> wrt;
    };
    Task<bidi_stream_state> bidi_stream_call(
        const std::string& service,
        const std::string& method);

    // Close the channel
    void close();

    // Check if connected
    bool is_connected() const { return connected_; }

private:
    struct h2_state {
        http::http2_session session{http::http2_session::mode::client};
        std::string host;

        explicit h2_state(const std::string& h) : host(h) {}
    };

    io_context& ctx_;
    std::string host_;
    uint16_t port_;
    tcp::socket sock_;
    bool connected_ = false;
    std::shared_ptr<h2_state> h2_;

    Task<void> read_loop();
    Task<void>* read_task_ = nullptr;
};

} // namespace async_net::frpc
