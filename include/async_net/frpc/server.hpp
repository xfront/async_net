// FRPC Server — FlatBuffers RPC server built on HTTP/2
// Supports Unary, Server Streaming, Client Streaming, and Bidirectional Streaming RPCs.
// Uses FlatBuffers serialization instead of Protocol Buffers.
#pragma once

#include <async_net/coroutine/task.hpp>
#include <async_net/frpc/types.hpp>
#include <async_net/grpc/interceptor.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/io/ssl.hpp>
#endif

namespace async_net::frpc {

// ============================================================================
// FRPC Method Handlers (4 RPC types)
// Handlers receive/return raw bytes (FlatBuffers serialized data)
// ============================================================================

// Unary: request -> response
using method_handler = std::function<Task<std::string>(const std::string&)>;

// Unary with call context (includes metadata)
using method_handler_with_context = std::function<Task<std::string>(const std::string&, call_context&)>;

// Server streaming: request -> multiple responses via writer
using server_stream_handler = std::function<Task<void>(const std::string& request, writer& w)>;

// Server streaming with context
using server_stream_handler_with_context =
    std::function<Task<void>(const std::string& request, writer& w, call_context&)>;

// Client streaming: multiple requests via reader -> response
using client_stream_handler = std::function<Task<std::string>(reader& r)>;

// Client streaming with context
using client_stream_handler_with_context = std::function<Task<std::string>(reader& r, call_context&)>;

// Bidirectional streaming: reader/writer
using bidi_stream_handler = std::function<Task<void>(reader& r, writer& w)>;

// Bidirectional streaming with context
using bidi_stream_handler_with_context = std::function<Task<void>(reader& r, writer& w, call_context&)>;

// ============================================================================
// FRPC Server
// ============================================================================

class server {
public:
    // Construct a FRPC server bound to the given io_context and port
    server(io_context& ctx, uint16_t port, const char* addr = "0.0.0.0");

    // Register method handlers
    void register_method(const std::string& service, const std::string& method, method_handler handler);
    void register_method(const std::string& service, const std::string& method, method_handler_with_context handler);
    void register_server_stream(const std::string& service, const std::string& method, server_stream_handler handler);
    void register_server_stream(const std::string& service, const std::string& method,
                                server_stream_handler_with_context handler);
    void register_client_stream(const std::string& service, const std::string& method, client_stream_handler handler);
    void register_client_stream(const std::string& service, const std::string& method,
                                client_stream_handler_with_context handler);
    void register_bidi_stream(const std::string& service, const std::string& method, bidi_stream_handler handler);
    void register_bidi_stream(const std::string& service, const std::string& method,
                              bidi_stream_handler_with_context handler);

    // Add a server interceptor
    void add_interceptor(grpc::interceptor_fn interceptor);

    // Start serving (plain HTTP/2 with prior knowledge)
    Task<void> serve();

#ifdef ASYNC_NET_HAS_SSL
    // Start serving with TLS
    Task<void> serve_tls(ssl::context& ssl_ctx);
#endif

    // Stop the server
    void stop();

private:
    enum class handler_type { unary, server_stream, client_stream, bidi_stream };

    struct method_entry {
        handler_type type;
        bool has_context = false;
        method_handler unary;
        method_handler_with_context unary_ctx;
        server_stream_handler srv_stream;
        server_stream_handler_with_context srv_stream_ctx;
        client_stream_handler cli_stream;
        client_stream_handler_with_context cli_stream_ctx;
        bidi_stream_handler bidi;
        bidi_stream_handler_with_context bidi_ctx;
    };

    using method_map = std::map<std::string, method_entry>;

    io_context& ctx_;
    tcp::acceptor acceptor_;
    uint16_t port_;
    method_map methods_;
    grpc::server_interceptor_chain interceptors_;
    bool running_ = true;

    Task<void> handle_connection(tcp::socket sock);

#ifdef ASYNC_NET_HAS_SSL
    Task<void> handle_tls_connection(tcp::socket sock, ssl::context& ssl_ctx);
#endif

    Task<std::pair<status, std::string>> dispatch(const std::string& path, const std::string& data);
    Task<std::pair<status, std::string>> dispatch(const std::string& path, const std::string& data, call_context& ctx);
};

}  // namespace async_net::frpc
