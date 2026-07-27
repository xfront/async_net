#pragma once

// ---------------------------------------------------------------------------
// tls_handler — TLS/SSL Service Handler for Acceptor & Connector patterns
//
// Inherits from service_handler<tcp::socket>, handles TLS handshake
// automatically, then delegates protocol logic to run_tls(ssl::stream&).
//
// The subclass owns the shutdown/close lifecycle inside run_tls().
//
// Usage with Acceptor (server-side):
//   class echo_tls_handler : public network::tls_handler {
//   public:
//       echo_tls_handler(ssl::context& ctx) : tls_handler(ctx, true) {}
//       Task<void> run_tls(ssl::stream& strm) override { /* echo */ }
//   };
//
//   struct echo_acc : network::acceptor<echo_tls_handler> {
//       ssl::context& ctx;
//       echo_acc(io_context& io, uint16_t p, ssl::context& c)
//           : network::acceptor<echo_tls_handler>(io, p), ctx(c) {}
//       pointer make_handler() override {
//           return std::make_shared<echo_tls_handler>(ctx);
//       }
//   };
//
// Usage with Connector (client-side):
//   network::connector<echo_tls_handler> conn(io_ctx);
//   conn.set_factory([&ssl_ctx]() {
//       return std::make_shared<echo_tls_handler>(ssl_ctx, false);
//   });
//   co_await conn.connect("host", 443);
// ---------------------------------------------------------------------------

#include <async_net/network/service_handler.hpp>
#include <async_net/io/ssl.hpp>

namespace async_net::network {

/// TLS/SSL Service Handler base class.
///
/// Automatically performs TLS handshake on run(), then delegates
/// the protocol logic to run_tls(ssl::stream&).
///
/// The subclass is responsible for cleanup (stream shutdown / socket close)
/// inside run_tls().
class tls_handler : public service_handler<tcp::socket> {
public:
    /// @param ssl_ctx  SSL context (must outlive the handler)
    /// @param is_server true for server-side (accept), false for client (connect)
    explicit tls_handler(ssl::context& ssl_ctx, bool is_server = true)
        : ssl_ctx_(&ssl_ctx), is_server_(is_server) {}

    /// TLS handshake → run_tls(strm).
    /// If handshake fails, closes peer and returns.
    Task<void> run() override {
        if (!peer_.has_value()) co_return;

        ssl::stream strm(*peer_, *ssl_ctx_, is_server_);
        auto hs = co_await strm.async_handshake();
        if (hs <= 0) {
            if (peer_) peer_->close();
            co_return;
        }

        co_await run_tls(strm);
    }

    /// Override to implement custom protocol logic over TLS.
    /// The stream is already handshaked when this is called.
    /// Subclass handles cleanup (shutdown, close) inside this method.
    virtual Task<void> run_tls(ssl::stream& strm) = 0;

protected:
    ssl::context* ssl_ctx_;
    bool is_server_;
};

} // namespace async_net::network
