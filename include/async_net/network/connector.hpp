#pragma once

// ---------------------------------------------------------------------------
// connector — Connector pattern (active connection establishment)
//
// Initiates outbound connections and creates a Service Handler
// for each successfully established connection.
//
// Template parameters:
//   SVC_HANDLER    — subclass of service_handler that processes connections
//   PEER_CONNECTOR — the connector/stream type (default: tcp::socket)
//
// PEER_CONNECTOR must satisfy:
//   - Constructable with (io_context&)
//   - async_connect(host, port) -> Task<int>
//   - Must be movable and match SVC_HANDLER::peer_stream_type
//
// Usage:
//   class my_handler : public network::service_handler<> { ... };
//   network::connector<my_handler> conn(ctx);
//   int ret = co_await conn.connect("example.com", 8080);
//
// For custom transport (e.g., TLS):
//   network::connector<my_tls_handler, ssl::stream> conn(ctx);
// ---------------------------------------------------------------------------

#include <async_net/network/service_handler.hpp>
#include <async_net/coroutine/task.hpp>
#include <memory>
#include <type_traits>

namespace async_net::network {

template<typename SVC_HANDLER, typename PEER_CONNECTOR = tcp::socket>
class connector {
    static_assert(std::is_base_of_v<service_handler<typename SVC_HANDLER::peer_stream_type>, SVC_HANDLER>,
                  "SVC_HANDLER must inherit from service_handler");
    static_assert(std::is_same_v<typename SVC_HANDLER::peer_stream_type, PEER_CONNECTOR>,
                  "SVC_HANDLER::peer_stream_type must match PEER_CONNECTOR");

public:
    using handler_type = SVC_HANDLER;
    using peer_connector_type = PEER_CONNECTOR;
    using peer_stream_type = typename handler_type::peer_stream_type;
    using pointer = std::shared_ptr<handler_type>;

    explicit connector(io_context& ctx) : ctx_(&ctx) {}

    virtual ~connector() = default;

    /// Factory method — override to customize handler creation.
    virtual pointer make_handler() {
        return std::make_shared<handler_type>();
    }

    /// Connect to host:port and run the service handler.
    /// Returns 0 on success, -1 on connection failure.
    Task<int> connect(const char* host, uint16_t port) {
        auto handler = make_handler();

        PEER_CONNECTOR peer(*ctx_);
        int ret = co_await peer.async_connect(host, port);
        if (ret < 0) {
            co_return -1;
        }

        handler->open(std::move(peer));
        co_await handler->run();
        co_return 0;
    }

    io_context& get_io_context() { return *ctx_; }

private:
    io_context* ctx_;
};

} // namespace async_net::network
