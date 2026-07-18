#include <async_net/http/client.hpp>
#include "../http/http1_codec.hpp"
#include <iostream>

namespace async_net {
namespace http {

// ---------------------------------------------------------------------------
// Constructors / Destructor
// ---------------------------------------------------------------------------

client::client(io_context& ctx) : ctx_(ctx) {}

#ifdef ASYNC_NET_HAS_SSL
client::client(io_context& ctx, ssl::context& ssl_ctx)
    : ctx_(ctx), ssl_ctx_(&ssl_ctx) {}
#endif

client::~client() {
    close_idle_connections();
}

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------

std::string client::pool_key(const std::string& host, uint16_t port, bool tls) {
    return host + ":" + std::to_string(port) + ":" + (tls ? "1" : "0");
}

void client::return_h1(const std::string& key, std::unique_ptr<h1_conn> conn) {
    h1_pool_[key].push_back(std::move(conn));
}

std::unique_ptr<client::h1_conn> client::take_h1(const std::string& key) {
    auto it = h1_pool_.find(key);
    if (it == h1_pool_.end() || it->second.empty()) return nullptr;
    auto conn = std::move(it->second.back());
    it->second.pop_back();
    if (it->second.empty()) h1_pool_.erase(it);
    return conn;
}

void client::close_idle_connections() {
    h1_pool_.clear();
#ifdef ASYNC_NET_HAS_SSL
    h2_pool_.clear();
#endif
}

// ---------------------------------------------------------------------------
// Convenience methods
// ---------------------------------------------------------------------------

Task<response> client::get(const std::string& url) {
    uri u(url);
    request req = request_make()
        .method_(method::GET)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header("Host", u.host())
        .header("Connection", "keep-alive")
        .header("User-Agent", "async_net/1.0")
        .build();

    co_return co_await send(u.host(), u.port(), std::move(req), u.is_https());
}

Task<response> client::post(const std::string& url, std::string body_text,
                             const std::string& content_type) {
    uri u(url);
    request req = request_make()
        .method_(method::POST)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header("Host", u.host())
        .header("Content-Type", content_type)
        .header("Connection", "keep-alive")
        .header("User-Agent", "async_net/1.0")
        .body(std::move(body_text))
        .build();

    co_return co_await send(u.host(), u.port(), std::move(req), u.is_https());
}

Task<response> client::put(const std::string& url, std::string body_text,
                            const std::string& content_type) {
    uri u(url);
    request req = request_make()
        .method_(method::PUT)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header("Host", u.host())
        .header("Content-Type", content_type)
        .header("Connection", "keep-alive")
        .header("User-Agent", "async_net/1.0")
        .body(std::move(body_text))
        .build();

    co_return co_await send(u.host(), u.port(), std::move(req), u.is_https());
}

Task<response> client::delete_(const std::string& url) {
    uri u(url);
    request req = request_make()
        .method_(method::DELETE_)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header("Host", u.host())
        .header("Connection", "keep-alive")
        .header("User-Agent", "async_net/1.0")
        .build();

    co_return co_await send(u.host(), u.port(), std::move(req), u.is_https());
}

// ---------------------------------------------------------------------------
// General send
// ---------------------------------------------------------------------------

Task<response> client::send(request req) {
    auto host_hdr = req.hdrs.get("Host");
    if (!host_hdr) {
        co_return response_bad_request("Missing Host header");
    }

    uri u("http://" + *host_hdr + req.path);
    co_return co_await send(u.host(), u.port(), std::move(req), false);
}

Task<response> client::send(const std::string& host, uint16_t port,
                             request req, bool use_tls) {
    std::string key = pool_key(host, port, use_tls);

    // Ensure Host header
    if (!req.hdrs.contains("Host")) {
        req.hdrs.set("Host", host);
    }

    // ===================================================================
    // TLS path: try H2 session pool first, then H1 pool, then new conn
    // ===================================================================
#ifdef ASYNC_NET_HAS_SSL
    if (use_tls && ssl_ctx_) {
        // --- Try reusing an H2 session ---
        {
            auto it = h2_pool_.find(key);
            while (it != h2_pool_.end() && !it->second.empty()) {
                auto h2 = std::move(it->second.back());
                it->second.pop_back();
                if (it->second.empty()) h2_pool_.erase(it);

                if (!h2->session || !h2->session->is_alive()) continue;

                // H2 multiplex: submit request on existing session
                auto send_fn = [&ssl_stream = *h2->ssl_stream](const uint8_t* data, size_t len) -> Task<ssize_t> {
                    co_return co_await ssl_stream.async_write_some(const_buffer(data, len));
                };

                request h2_req = req;
                h2_req.ver = version::HTTP_2;
                if (!h2_req.hdrs.contains(":authority")) {
                    h2_req.hdrs.set(":authority", host);
                }

                auto promise = std::make_shared<http2_session::response_promise>();
                h2->session->submit_request(h2_req, promise);
                co_await h2->session->flush(send_fn);

                // Read until response completes
                char read_buf[16384];
                while (!promise->complete && h2->session->is_alive()) {
                    auto n = co_await h2->ssl_stream->async_read_some(
                        mutable_buffer(read_buf, sizeof(read_buf)));
                    if (n <= 0) break;
                    h2->session->feed(reinterpret_cast<const uint8_t*>(read_buf),
                                      static_cast<size_t>(n));
                    co_await h2->session->flush(send_fn);
                }

                if (promise->complete && !promise->error) {
                    response resp = std::move(promise->resp);
                    resp.ver = version::HTTP_2;
                    // Return H2 session to pool for reuse
                    if (h2->session->is_alive()) {
                        h2_pool_[key].push_back(std::move(h2));
                    }
                    co_return resp;
                }
                // H2 session broken, try next or create new
            }
        }

        // --- Try reusing an H1 keep-alive connection ---
        {
            auto h1 = take_h1(key);
            if (h1 && h1->is_tls && h1->ssl_stream) {
                // Send request on existing connection
                std::string data = http1_codec::serialize(req);
                size_t total_sent = 0;
                bool write_ok = true;
                while (total_sent < data.size()) {
                    auto n = co_await h1->ssl_stream->async_write_some(
                        const_buffer(data.data() + total_sent, data.size() - total_sent));
                    if (n <= 0) { write_ok = false; break; }
                    total_sent += static_cast<size_t>(n);
                }

                if (write_ok) {
                    auto resp = co_await read_response_ssl(*h1->ssl_stream);
                    if (resp.has_value()) {
                        // Return to pool if keep-alive
                        auto conn_hdr = resp->hdrs.get("Connection");
                        bool keep = (conn_hdr && *conn_hdr == "keep-alive") ||
                                    resp->ver == version::HTTP_11;
                        if (keep) {
                            return_h1(key, std::move(h1));
                        }
                        co_return std::move(*resp);
                    }
                }
                // Connection stale, fall through to create new
            }
        }

        // --- Create new TLS connection ---
        {
            auto conn = std::make_unique<h1_conn>();
            conn->sock = std::make_unique<tcp::socket>(ctx_);

            auto ret = co_await conn->sock->async_connect(host.c_str(), port);
            if (ret < 0) {
                co_return response_internal_error("Connection failed to " + host + ":" + std::to_string(port));
            }

            // Heap-allocate ssl::stream pointing to heap-allocated socket
            conn->ssl_stream = std::make_unique<ssl::stream>(*conn->sock, *ssl_ctx_, false);

            // Set ALPN to negotiate h2 + http/1.1
            ssl_ctx_->set_alpn_protos({"h2", "http/1.1"});

            auto hs = co_await conn->ssl_stream->async_handshake();
            if (hs <= 0) {
                co_return response_internal_error("TLS handshake failed");
            }

            std::string alpn = conn->ssl_stream->alpn_selected();
            conn->is_tls = true;

            if (alpn == "h2") {
                // --- H2 mode: set up session and send request ---
                auto h2 = std::make_unique<h2_conn>();
                h2->sock = std::move(conn->sock);
                h2->ssl_stream = std::move(conn->ssl_stream);
                h2->session = std::make_unique<http2_session>(http2_session::mode::client);

                auto send_fn = [&ssl_stream = *h2->ssl_stream](const uint8_t* data, size_t len) -> Task<ssize_t> {
                    co_return co_await ssl_stream.async_write_some(const_buffer(data, len));
                };

                // Send H2 connection preface
                static const char* H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
                size_t preface_sent = 0;
                while (preface_sent < 24) {
                    auto n = co_await h2->ssl_stream->async_write_some(
                        const_buffer(H2_PREFACE + preface_sent, 24 - preface_sent));
                    if (n <= 0) {
                        co_return response_make().status(status_code::internal_error())
                            .body("Failed to send H2 preface").build();
                    }
                    preface_sent += static_cast<size_t>(n);
                }

                // Send client SETTINGS
                co_await h2->session->flush(send_fn);

                // Submit request
                request h2_req = req;
                h2_req.ver = version::HTTP_2;
                if (!h2_req.hdrs.contains(":authority")) {
                    h2_req.hdrs.set(":authority", host);
                }

                auto promise = std::make_shared<http2_session::response_promise>();
                h2->session->submit_request(h2_req, promise);
                co_await h2->session->flush(send_fn);

                // Read until response
                char read_buf[16384];
                while (!promise->complete && h2->session->is_alive()) {
                    auto n = co_await h2->ssl_stream->async_read_some(
                        mutable_buffer(read_buf, sizeof(read_buf)));
                    if (n <= 0) break;
                    h2->session->feed(reinterpret_cast<const uint8_t*>(read_buf),
                                      static_cast<size_t>(n));
                    co_await h2->session->flush(send_fn);
                }

                if (promise->complete && !promise->error) {
                    response resp = std::move(promise->resp);
                    resp.ver = version::HTTP_2;
                    // Store H2 session in pool for future reuse
                    if (h2->session->is_alive()) {
                        h2_pool_[key].push_back(std::move(h2));
                    }
                    co_return resp;
                }

                co_return response_make().status(status_code::internal_error())
                    .body("H2 request incomplete").build();
            } else {
                // --- HTTP/1.1 over TLS ---
                std::string data = http1_codec::serialize(req);
                size_t total_sent = 0;
                while (total_sent < data.size()) {
                    auto n = co_await conn->ssl_stream->async_write_some(
                        const_buffer(data.data() + total_sent, data.size() - total_sent));
                    if (n <= 0) {
                        co_return response_internal_error("TLS write failed");
                    }
                    total_sent += static_cast<size_t>(n);
                }

                auto resp = co_await read_response_ssl(*conn->ssl_stream);

                if (!resp.has_value()) {
                    co_return response_internal_error("Failed to read response");
                }

                // Return to pool if keep-alive
                auto conn_hdr = resp->hdrs.get("Connection");
                bool keep = !conn_hdr || *conn_hdr != "close";
                if (keep) {
                    return_h1(key, std::move(conn));
                }

                co_return std::move(*resp);
            }
        }
    }
#endif

    // ===================================================================
    // Plain HTTP path (no TLS)
    // ===================================================================

    // --- Try h2c upgrade (HTTP/1.1 → H2 via Upgrade header) ---
    // First request uses HTTP/1.1 with Upgrade: h2 header
    // If server responds with 101 Switching Protocols, we upgrade to H2

    // --- Try reusing an H1 keep-alive connection ---
    {
        auto h1 = take_h1(key);
        if (h1 && !h1->is_tls) {
            std::string data = http1_codec::serialize(req);
            size_t total_sent = 0;
            bool write_ok = true;
            while (total_sent < data.size()) {
                auto n = co_await h1->sock->async_write_some(
                    const_buffer(data.data() + total_sent, data.size() - total_sent));
                if (n <= 0) { write_ok = false; break; }
                total_sent += static_cast<size_t>(n);
            }

            if (write_ok) {
                auto resp = co_await read_response(*h1->sock);
                if (resp.has_value()) {
                    auto conn_hdr = resp->hdrs.get("Connection");
                    bool keep = (conn_hdr && *conn_hdr == "keep-alive") ||
                                resp->ver == version::HTTP_11;
                    if (keep) {
                        return_h1(key, std::move(h1));
                    }
                    co_return std::move(*resp);
                }
            }
            // Connection stale, fall through
        }
    }

    // --- Create new plain HTTP connection ---
    {
        auto conn = std::make_unique<h1_conn>();
        conn->sock = std::make_unique<tcp::socket>(ctx_);

        auto ret = co_await conn->sock->async_connect(host.c_str(), port);
        if (ret < 0) {
            co_return response_internal_error("Connection failed to " + host + ":" + std::to_string(port));
        }

#ifdef ASYNC_NET_HAS_SSL
        // Add h2c Upgrade header for potential H2 upgrade
        req.hdrs.set("Upgrade", "h2c");
        req.hdrs.set("HTTP2-Settings", "AAMAAABkAAQBAAAAAAIAAAAA");
        req.hdrs.set("Connection", "Upgrade,HTTP2-Settings");
#endif

        std::string data = http1_codec::serialize(req);
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            auto n = co_await conn->sock->async_write_some(
                const_buffer(data.data() + total_sent, data.size() - total_sent));
            if (n <= 0) {
                co_return response_internal_error("Write failed");
            }
            total_sent += static_cast<size_t>(n);
        }

        // Read response
        auto resp = co_await read_response(*conn->sock);

        if (!resp.has_value()) {
            co_return response_internal_error("Failed to read response");
        }

#ifdef ASYNC_NET_HAS_SSL
        // Check for 101 Switching Protocols (h2c upgrade success)
        if (resp->status.as_int() == 101) {
            // Server accepted h2c upgrade — switch to H2 mode
            auto h2 = std::make_unique<h2_conn>();
            h2->sock = std::move(conn->sock);
            h2->session = std::make_unique<http2_session>(http2_session::mode::client);

            auto send_fn = [&sock = *h2->sock](const uint8_t* data, size_t len) -> Task<ssize_t> {
                co_return co_await sock.async_write_some(const_buffer(data, len));
            };

            // Send client SETTINGS (after upgrade)
            co_await h2->session->flush(send_fn);

            // Re-submit the request on H2
            request h2_req = req;
            h2_req.ver = version::HTTP_2;
            // Remove h2c upgrade headers
            h2_req.hdrs.remove("Upgrade");
            h2_req.hdrs.remove("HTTP2-Settings");
            h2_req.hdrs.remove("Connection");
            if (!h2_req.hdrs.contains(":authority")) {
                h2_req.hdrs.set(":authority", host);
            }

            auto promise = std::make_shared<http2_session::response_promise>();
            h2->session->submit_request(h2_req, promise);
            co_await h2->session->flush(send_fn);

            // Read until response
            char read_buf[16384];
            while (!promise->complete && h2->session->is_alive()) {
                auto n = co_await h2->sock->async_read_some(
                    mutable_buffer(read_buf, sizeof(read_buf)));
                if (n <= 0) break;
                h2->session->feed(reinterpret_cast<const uint8_t*>(read_buf),
                                  static_cast<size_t>(n));
                co_await h2->session->flush(send_fn);
            }

            if (promise->complete && !promise->error) {
                response h2_resp = std::move(promise->resp);
                h2_resp.ver = version::HTTP_2;
                // Note: plain H2 connections can't easily be pooled without TLS
                co_return h2_resp;
            }

            co_return response_make().status(status_code::internal_error())
                .body("h2c upgrade request incomplete").build();
        }
#endif

        // Normal HTTP/1.1 response
        // Return to pool if keep-alive
        auto conn_hdr = resp->hdrs.get("Connection");
        bool keep = !conn_hdr || *conn_hdr != "close";
        if (keep) {
            return_h1(key, std::move(conn));
        }

        co_return std::move(*resp);
    }
}

// ---------------------------------------------------------------------------
// Read response from plain socket
// ---------------------------------------------------------------------------

Task<std::optional<response>> client::read_response(tcp::socket& sock) {
    std::string buffer;
    char read_buf[8192];

    for (int attempts = 0; attempts < 1000; ++attempts) {
        auto result = http1_codec::parse_response(buffer);
        if (result.has_value()) {
            co_return result->first;
        }

        auto n = co_await sock.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) {
            if (!buffer.empty()) {
                auto last_try = http1_codec::parse_response(buffer);
                if (last_try.has_value()) {
                    co_return last_try->first;
                }
            }
            co_return std::nullopt;
        }

        buffer.append(read_buf, static_cast<size_t>(n));
    }

    co_return std::nullopt;
}

#ifdef ASYNC_NET_HAS_SSL
Task<std::optional<response>> client::read_response_ssl(ssl::stream& stream) {
    std::string buffer;
    char read_buf[8192];

    for (int attempts = 0; attempts < 1000; ++attempts) {
        auto result = http1_codec::parse_response(buffer);
        if (result.has_value()) {
            co_return result->first;
        }

        auto n = co_await stream.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) {
            if (!buffer.empty()) {
                auto last_try = http1_codec::parse_response(buffer);
                if (last_try.has_value()) {
                    co_return last_try->first;
                }
            }
            co_return std::nullopt;
        }

        buffer.append(read_buf, static_cast<size_t>(n));
    }

    co_return std::nullopt;
}
#endif

} // namespace http
} // namespace async_net
