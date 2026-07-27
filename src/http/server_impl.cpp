#include <async_net/http/server.hpp>
#include <async_net/http/server_builder.hpp>
#include <async_net/network/connector.hpp>
#include <async_net/executor/process_manager.hpp>
#include <cstring>
#include <iostream>

#include "../http/http1_codec.hpp"

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/io/udp.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <arpa/inet.h>
#endif
#endif

namespace async_net::http {

// ===========================================================================
// Helper functions
// ===========================================================================

static Task<response> dispatch_request(server_context& ctx, const request& req) {
    for (auto& r : ctx.routes) {
        if (r.m == req.method && r.path == req.path) {
            co_return co_await r.handler(req);
        }
    }
    co_return co_await ctx.default_handler(req);
}

static ws::ws_handler_fn* find_ws_handler(server_context& ctx, const std::string& path) {
    for (auto& r : ctx.ws_routes) {
        if (r.first == path)
            return &r.second;
    }
    return nullptr;
}

// ===========================================================================
// Shared HTTP/1.1 keep-alive loop (template, used by both plain and TLS)
// ===========================================================================
//
// Stream must support async_read_some(mutable_buffer) -> Task<ssize_t>
//               and async_write_some(const_buffer)    -> Task<ssize_t>

template <typename Stream>
static Task<void> http11_keep_alive_loop(server_context& ctx, tcp::socket& sock, Stream& stream) {
    std::string buffer;
    char read_buf[8192];

    while (ctx.running.load(std::memory_order_relaxed)) {
        buffer.clear();
        std::optional<request> req_opt;

        for (int attempts = 0; attempts < 1000; ++attempts) {
            auto result = http1_codec::parse_request(buffer);
            if (result.has_value()) {
                req_opt = std::move(result->first);
                break;
            }
            auto n = co_await stream.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
            if (n <= 0)
                break;
            buffer.append(read_buf, static_cast<size_t>(n));
        }

        if (!req_opt.has_value())
            break;

        // Check for WebSocket upgrade
        if (ws::is_websocket_upgrade(*req_opt)) {
            auto* ws_handler = find_ws_handler(ctx, req_opt->path);
            if (ws_handler) {
                // Simple WebSocket handshake + run
                std::string handshake = ws::build_handshake_response(*req_opt);
                size_t total_sent = 0;
                while (total_sent < handshake.size()) {
                    auto n = co_await stream.async_write_some(
                        const_buffer(handshake.data() + total_sent, handshake.size() - total_sent));
                    if (n <= 0)
                        break;
                    total_sent += static_cast<size_t>(n);
                }
                ws::websocket_connection ws_conn(sock);
                co_await (*ws_handler)(ws_conn);
                break;
            }
        }

        bool keep_alive = req_opt->hdrs.is_keep_alive() && req_opt->ver == version::HTTP_11;
        response resp = co_await dispatch_request(ctx, *req_opt);

        if (keep_alive)
            resp.hdrs.set("Connection", "keep-alive");
        else
            resp.hdrs.set("Connection", "close");

#ifdef ASYNC_NET_HAS_HTTP3
        if (!resp.hdrs.contains("Alt-Svc")) {
            resp.hdrs.set("Alt-Svc", "h3=\":" + std::to_string(ctx.port) + "\"; ma=86400");
        }
#endif

        if (!resp.hdrs.contains("Content-Length")) {
            resp.hdrs.set("Content-Length", std::to_string(resp.bd.size()));
        }

        std::string data = http1_codec::serialize(resp);
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            auto n = co_await stream.async_write_some(const_buffer(data.data() + total_sent, data.size() - total_sent));
            if (n <= 0)
                goto done;
            total_sent += static_cast<size_t>(n);
        }

        if (!keep_alive)
            break;
    }

done:
    sock.close();
}

struct plain_acceptor : network::acceptor<http11_handler> {
    std::shared_ptr<server_context> sc;
    plain_acceptor(io_context& io, uint16_t p, const char* a, bool rp, std::shared_ptr<server_context> c)
        : network::acceptor<http11_handler>(io, p, a, rp), sc(c) {}
    handler_ptr make_handler() override { return std::make_shared<http11_handler>(sc); }
};

#ifdef ASYNC_NET_HAS_SSL
struct tls_acceptor : network::acceptor<https_handler> {
    std::shared_ptr<server_context> ctx;
    ssl::context* ssl_ctx_ptr;
    tls_acceptor(io_context& io, uint16_t p, const char* a, bool rp, std::shared_ptr<server_context> c,
                 ssl::context& ssl)
        : network::acceptor<https_handler>(io, p, a, rp), ctx(c), ssl_ctx_ptr(&ssl) {}
    handler_ptr make_handler() override { return std::make_shared<https_handler>(ctx, *ssl_ctx_ptr); }
};
#endif

// ===========================================================================
// http11_handler::run() — plain HTTP/1.1
// ===========================================================================

Task<void> http11_handler::run() {
    co_await http11_keep_alive_loop(*ctx_, *peer_, *peer_);
}

// ===========================================================================
// https_handler::run_tls() — TLS + ALPN (H2 + H1.1)
// ===========================================================================

#ifdef ASYNC_NET_HAS_SSL
// Synchronous request dispatcher for H2 (routes call coroutine handlers)
struct h2_dispatch_adapter {
    server_context& ctx;

    response dispatch(const request& req) {
        response resp;
        bool found = false;
        for (auto& r : ctx.routes) {
            if (r.m == req.method && r.path == req.path) {
                auto task = r.handler(req);
                task.resume();
                if (task.done()) {
                    resp = task.handle().promise().result();
                    found = true;
                } else {
                    return response_internal_error("Handler not synchronous");
                }
                break;
            }
        }
        if (!found) {
            auto task = ctx.default_handler(req);
            task.resume();
            if (task.done())
                resp = task.handle().promise().result();
            else
                return response_not_found();
        }
#ifdef ASYNC_NET_HAS_HTTP3
        if (!resp.hdrs.contains("Alt-Svc")) {
            resp.hdrs.set("Alt-Svc", "h3=\":" + std::to_string(ctx.port) + "\"; ma=86400");
        }
#endif
        return resp;
    }
};

Task<void> https_handler::run_tls(ssl::stream& strm) {
    std::string alpn = strm.alpn_selected();

    if (alpn == "h2") {
        // HTTP/2 mode
        http2_session session(http2_session::mode::server);
        h2_dispatch_adapter adapter{*ctx_};

        session.set_request_handler([&adapter](const request& req) -> response { return adapter.dispatch(req); });

        if (ctx_->push_provider_fn) {
            session.set_push_provider(ctx_->push_provider_fn);
        }

        auto send_fn = [&strm](const uint8_t* data, size_t len) -> Task<ssize_t> {
            co_return co_await strm.async_write_some(const_buffer(data, len));
        };
        co_await session.flush(send_fn);

        // Read client connection preface (24 bytes)
        {
            static constexpr const char* PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            static constexpr size_t PREFACE_LEN = 24;
            char preface_buf[256];
            size_t preface_read = 0;
            while (preface_read < PREFACE_LEN) {
                auto n = co_await strm.async_read_some(
                    mutable_buffer(preface_buf + preface_read, sizeof(preface_buf) - preface_read));
                if (n <= 0)
                    break;
                preface_read += static_cast<size_t>(n);
            }
            if (preface_read < PREFACE_LEN || std::memcmp(preface_buf, PREFACE, PREFACE_LEN) != 0) {
                std::cerr << "[h2] Invalid client connection preface" << std::endl;
                co_await strm.async_shutdown();
                peer_->close();
                co_return;
            }
            if (preface_read > PREFACE_LEN) {
                session.feed(reinterpret_cast<const uint8_t*>(preface_buf + PREFACE_LEN), preface_read - PREFACE_LEN);
            }
        }

        co_await session.flush(send_fn);

        char read_buf[16384];
        while (session.is_alive() && ctx_->running.load(std::memory_order_relaxed)) {
            auto n = co_await strm.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
            if (n <= 0)
                break;
            auto consumed = session.feed(reinterpret_cast<const uint8_t*>(read_buf), static_cast<size_t>(n));
            if (consumed < 0)
                break;
            bool ok = co_await session.flush(send_fn);
            if (!ok)
                break;
        }

        co_await strm.async_shutdown();
    } else {
        // Fallback to HTTP/1.1 over TLS
        co_await http11_keep_alive_loop(*ctx_, *peer_, strm);
        co_await strm.async_shutdown();
    }

    peer_->close();
}
#endif  // ASYNC_NET_HAS_SSL

// ===========================================================================
// server implementation
// ===========================================================================

server::server(io_context& ctx, uint16_t port, const char* addr, bool reuse_port)
    : ctx_(std::make_shared<server_context>()) {
    ctx_->io_ctx = &ctx;
    ctx_->port = port;
    ctx_->addr = addr ? addr : "0.0.0.0";
    ctx_->reuse_port = reuse_port;
    ctx_->default_handler = [](const request&) -> Task<response> { co_return response_not_found(); };
}

void server::route(method m, const std::string& path, handler_fn handler) {
    ctx_->routes.push_back({m, path, std::move(handler)});
}

void server::ws_route(const std::string& path, ws::ws_handler_fn handler) {
    ctx_->ws_routes.emplace_back(path, std::move(handler));
}

void server::default_handler(handler_fn handler) { ctx_->default_handler = std::move(handler); }

void server::set_push_provider(push_provider provider) { ctx_->push_provider_fn = std::move(provider); }

// ---------------------------------------------------------------------------
// serve() — HTTP/1.1
// ---------------------------------------------------------------------------

Task<void> server::serve() {
    ctx_->h1_acceptor = std::make_unique<network::acceptor<http11_handler>>(*ctx_->io_ctx, ctx_->port,
                                                                            ctx_->addr.c_str(), ctx_->reuse_port);

    auto acc = std::make_unique<plain_acceptor>(*ctx_->io_ctx, ctx_->port, ctx_->addr.c_str(), ctx_->reuse_port, ctx_);

    // Transfer ownership to ctx_ for lifetime management
    ctx_->h1_acceptor = std::move(acc);
    co_await ctx_->h1_acceptor->serve();
}

// ---------------------------------------------------------------------------
// serve(ssl_ctx) — TLS + ALPN
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_SSL
Task<void> server::serve(ssl::context& ssl_ctx) {
    // Setup ALPN
    ssl_ctx.set_alpn_select_cb([](const std::vector<std::string>& protos) -> std::string {
        for (auto& p : protos) {
            if (p == "h2")
                return "h2";
        }
        for (auto& p : protos) {
            if (p == "http/1.1")
                return "http/1.1";
        }
        return "";
    });

    auto acc =
        std::make_unique<tls_acceptor>(*ctx_->io_ctx, ctx_->port, ctx_->addr.c_str(), ctx_->reuse_port, ctx_, ssl_ctx);
    ctx_->tls_acceptor = std::move(acc);
    co_await ctx_->tls_acceptor->serve();
}

Task<void> server::serve_h2(ssl::context& ssl_ctx) { co_await serve(ssl_ctx); }
#endif

// ---------------------------------------------------------------------------
// serve_h3() — HTTP/3 over QUIC/UDP
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_HTTP3
Task<void> server::serve_h3(const std::string& cert_file, const std::string& key_file) {
    auto& ctx = *ctx_;

    udp::socket udp_sock(*ctx.io_ctx);
    udp::endpoint ep(ctx.port);
    if (ctx.reuse_port)
        udp_sock.set_reuse_port(ctx.reuse_port);
    if (!udp_sock.bind(ep)) {
        std::cerr << "[http::server] Failed to bind UDP port " << ctx.port << std::endl;
        co_return;
    }

    std::cout << "[http::server] HTTP/3 listening on UDP port " << ctx.port << std::endl;

    http3_session::config cfg;
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    cfg.max_streams = 100;

    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(ctx.port);
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct conn_state {
        std::unique_ptr<http3_session> session;
        udp::endpoint remote;
    };
    std::unordered_map<std::string, conn_state> connections;

    auto ep_key = [](const udp::endpoint& e) -> std::string { return e.address() + ":" + std::to_string(e.port()); };

    h2_dispatch_adapter adapter{ctx};

    char buf[4096];
    udp::endpoint from;

    while (ctx.running.load(std::memory_order_relaxed)) {
        auto n = co_await udp_sock.async_receive_from(mutable_buffer(buf, sizeof(buf)), from);
        if (n <= 0)
            continue;

        std::string key = ep_key(from);

        for (auto it = connections.begin(); it != connections.end();) {
            if (it->second.session && !it->second.session->is_alive()) {
                it = connections.erase(it);
            } else {
                ++it;
            }
        }

        auto& cs = connections[key];

        if (!cs.session) {
            cs.session = std::make_unique<http3_session>(http3_session::mode::server, cfg);
            cs.remote = from;

            cs.session->set_request_handler(
                [&adapter](const request& req) -> response { return adapter.dispatch(req); });

            if (ctx.push_provider_fn) {
                cs.session->set_push_provider(ctx.push_provider_fn);
            }

            if (!cs.session->init_server_from_packet(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                                                     reinterpret_cast<const struct sockaddr*>(&local_addr),
                                                     sizeof(local_addr), from.sockaddr_ptr(), from.size())) {
                std::cerr << "[http::server] H3 init failed" << std::endl;
                cs.session.reset();
                continue;
            }

            cs.session->feed_packet(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                                    reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                                    from.sockaddr_ptr(), from.size());
        } else {
            cs.session->feed_packet(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                                    reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                                    from.sockaddr_ptr(), from.size());
        }

        auto pkts = cs.session->get_pending_packets();
        for (auto& pkt : pkts) {
            if (!pkt.empty()) {
                udp_sock.send_to(const_buffer(pkt.data(), pkt.size()), from);
            }
        }

        cs.session->handle_expiry();
    }

    udp_sock.close();
}
#endif

// ---------------------------------------------------------------------------
// serve_configured — uses stored configuration
// ---------------------------------------------------------------------------

Task<void> server::serve_configured() {
    // Launch H3 (QUIC/UDP) concurrently with TCP acceptors
#ifdef ASYNC_NET_HAS_HTTP3
    if (ctx_->h3_enabled) {
        ctx_->h3_task = std::make_unique<Task<void>>(serve_h3(ctx_->h3_cert, ctx_->h3_key));
        ctx_->h3_task->resume();
    }
#endif

    // Check if we have configured acceptors
    if (ctx_->h1_acceptor) {
        co_await ctx_->h1_acceptor->serve();
    }
#ifdef ASYNC_NET_HAS_SSL
    else if (ctx_->tls_acceptor) {
        co_await ctx_->tls_acceptor->serve();
    }
#endif
    else {
        // Default: H1
        co_await serve();
    }
}



void server::stop() {
    ctx_->running.store(false, std::memory_order_relaxed);
    if (ctx_->h1_acceptor)
        ctx_->h1_acceptor->stop();
#ifdef ASYNC_NET_HAS_SSL
    if (ctx_->tls_acceptor)
        ctx_->tls_acceptor->stop();
#endif
    ctx_->io_ctx->stop();
}

// ===========================================================================
// server_builder implementation
// ===========================================================================

std::unique_ptr<server> server_builder::build() {
    auto srv = std::make_unique<server>(*ctx_, port_, addr_.c_str(), reuse_port_);

    for (auto& r : routes_) {
        srv->route(r.m, r.path, r.handler);
    }
    for (auto& r : ws_routes_) {
        srv->ws_route(r.path, r.handler);
    }
    if (default_handler_) {
        srv->default_handler(default_handler_);
    }
    if (push_provider_) {
        srv->set_push_provider(push_provider_);
    }

#ifdef ASYNC_NET_HAS_SSL
    if (use_tls_ && ssl_ctx_) {
        // Set up ALPN in the ssl context
        ssl_ctx_->set_alpn_select_cb([](const std::vector<std::string>& protos) -> std::string {
            for (auto& p : protos) {
                if (p == "h2")
                    return "h2";
            }
            for (auto& p : protos) {
                if (p == "http/1.1")
                    return "http/1.1";
            }
            return "";
        });
        srv->ctx_->tls_acceptor =
            std::make_unique<tls_acceptor>(*ctx_, port_, addr_.c_str(), reuse_port_, srv->ctx_, *ssl_ctx_);
    } else
#endif
    {
        srv->ctx_->h1_acceptor = std::make_unique<plain_acceptor>(*ctx_, port_, addr_.c_str(), reuse_port_, srv->ctx_);
    }

    // H3 config stored in context for serve_configured()
#ifdef ASYNC_NET_HAS_HTTP3
    if (use_h3_) {
        srv->ctx_->h3_enabled = true;
        srv->ctx_->h3_cert = h3_cert_;
        srv->ctx_->h3_key = h3_key_;
    }
#endif

    return srv;
}

void server_builder::run(bool mp, unsigned num_workers) {
    // Set up ALPN callback on the shared SSL context (before spawning workers,
    // outside the worker function to avoid data races on the SSL context)
#ifdef ASYNC_NET_HAS_SSL
    if (use_tls_ && ssl_ctx_) {
        ssl_ctx_->set_alpn_select_cb([](const std::vector<std::string>& protos) -> std::string {
            for (auto& p : protos) {
                if (p == "h2")
                    return "h2";
            }
            for (auto& p : protos) {
                if (p == "http/1.1")
                    return "http/1.1";
            }
            return "";
        });
    }
#endif


    // Worker lambda — each thread creates its own server + acceptor
    auto worker = [this](int /*worker_id*/, io_context& ctx, const default_worker_config&) -> Task<void> {
        // Always use SO_REUSEPORT for multi-worker servers
        // (each thread creates its own acceptor on the same port)
        server srv(ctx, port_, addr_.c_str(), /*reuse_port=*/true);

        // Set up routes and handlers
        for (auto& r : routes_)
            srv.route(r.m, r.path, r.handler);
        for (auto& r : ws_routes_)
            srv.ws_route(r.path, r.handler);
        if (default_handler_)
            srv.default_handler(default_handler_);
        if (push_provider_)
            srv.set_push_provider(push_provider_);

        // Create the appropriate acceptor
#if defined(ASYNC_NET_HAS_SSL)
        if (use_tls_ && ssl_ctx_) {
            srv.ctx_->tls_acceptor =
                std::make_unique<tls_acceptor>(ctx, port_, addr_.c_str(), /*reuse_port=*/true, srv.ctx_, *ssl_ctx_);
        } else
#endif
        {
            srv.ctx_->h1_acceptor =
                std::make_unique<plain_acceptor>(ctx, port_, addr_.c_str(), /*reuse_port=*/true, srv.ctx_);
        }

        // H3 config
#if defined(ASYNC_NET_HAS_HTTP3)
        if (use_h3_) {
            srv.ctx_->h3_enabled = true;
            srv.ctx_->h3_cert = h3_cert_;
            srv.ctx_->h3_key = h3_key_;
        }
#endif

        co_await srv.serve_configured();
    };

    // Configure worker management via global run_mp_master_thread
    default_worker_config cfg;
    cfg.num_workers = num_workers;
    cfg.mode = mp? worker_mode::process: worker_mode::thread;
    run_mp_master(std::move(worker), cfg);
}

}  // namespace async_net::http
