#include <async_net/http/server.hpp>
#include "../http/http1_codec.hpp"
#include <iostream>
#include <cstring>
#include <unordered_map>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/http/http2_session.hpp>
#endif

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#include <async_net/net/udp.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <arpa/inet.h>
#endif
#endif

namespace async_net::http {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

server::server(io_context& ctx, uint16_t port, const char* addr, bool reuse_port)
    : ctx_(ctx), port_(port), addr_(addr ? addr : "0.0.0.0"), reuse_port_(reuse_port) {
    // Default 404 handler
    default_handler_ = [](const request&) -> Task<response> {
        co_return response_not_found();
    };
}

void server::route(method m, const std::string& path, handler_fn handler) {
    routes_.push_back({m, path, std::move(handler)});
}

void server::ws_route(const std::string& path, ws::ws_handler_fn handler) {
    ws_routes_.push_back({path, std::move(handler)});
}

void server::default_handler(handler_fn handler) {
    default_handler_ = std::move(handler);
}

void server::set_push_provider(push_provider provider) {
    push_provider_ = std::move(provider);
}

void server::stop() {
    running_ = false;
    if (acceptor_) {
        acceptor_->close();
    }
}

void server::cleanup_task(Task<void>* task) {
    std::lock_guard<std::mutex> lock(active_tasks_mutex_);
    active_tasks_.erase(task);
    // NOTE: do NOT delete here. The caller (serve/serve_tls/serve_h2) checks
    // task->done() and deletes. This avoids double-free when a coroutine
    // completes synchronously inside poll()'s mutex (before serve() can check).
}

// ---------------------------------------------------------------------------
// serve() — accept loop
// ---------------------------------------------------------------------------

Task<void> server::serve() {
    // Create acceptor lazily
    if (!acceptor_) {
        acceptor_ = std::make_unique<tcp::acceptor>(ctx_, port_, addr_.c_str(), reuse_port_);
    }
    if (!acceptor_->is_open()) {
        std::cerr << "[http::server] Failed to open acceptor" << std::endl;
        co_return;
    }

    std::cout << "[http::server] Listening on port" << std::endl;

    while (running_) {
        auto sock = co_await acceptor_->async_accept();
        if (!sock.is_open()) {
            if (!running_) break;
            continue;
        }

        auto* task = new Task<void>(handle_connection(std::move(sock)));
        {
            std::lock_guard<std::mutex> lock(active_tasks_mutex_);
            active_tasks_.insert(task);
        }
        task->resume();
        if (task->done()) {
            cleanup_task(task);
            delete task;
        }
    }
}

#ifdef ASYNC_NET_HAS_SSL
Task<void> server::serve_tls(ssl::context& ssl_ctx) {
    // Create acceptor lazily
    if (!acceptor_) {
        acceptor_ = std::make_unique<tcp::acceptor>(ctx_, port_, addr_.c_str(), reuse_port_);
    }
    if (!acceptor_->is_open()) {
        std::cerr << "[http::server] Failed to open acceptor" << std::endl;
        co_return;
    }

    std::cout << "[http::server] Listening on port (TLS)" << std::endl;

    while (running_) {
        auto sock = co_await acceptor_->async_accept();
        if (!sock.is_open()) {
            if (!running_) break;
            continue;
        }

        auto* task = new Task<void>(handle_tls_connection(std::move(sock), ssl_ctx));
        {
            std::lock_guard<std::mutex> lock(active_tasks_mutex_);
            active_tasks_.insert(task);
        }
        task->resume();
        if (task->done()) {
            cleanup_task(task);
            delete task;
        }
    }
}

Task<void> server::serve_h2(ssl::context& ssl_ctx) {
    // Set up ALPN for h2 + http/1.1
    ssl_ctx.set_alpn_select_cb([](const std::vector<std::string>& protos) -> std::string {
        for (auto& p : protos) {
            if (p == "h2") return "h2";
        }
        for (auto& p : protos) {
            if (p == "http/1.1") return "http/1.1";
        }
        return "";
    });

    // Create acceptor lazily
    if (!acceptor_) {
        acceptor_ = std::make_unique<tcp::acceptor>(ctx_, port_, addr_.c_str(), reuse_port_);
    }
    if (!acceptor_->is_open()) {
        std::cerr << "[http::server] Failed to open acceptor" << std::endl;
        co_return;
    }

    std::cout << "[http::server] Listening on port (H2 + TLS)" << std::endl;

    while (running_) {
        auto sock = co_await acceptor_->async_accept();
        if (!sock.is_open()) {
            if (!running_) break;
            continue;
        }

        auto* task = new Task<void>(handle_h2_connection(std::move(sock), ssl_ctx));
        {
            std::lock_guard<std::mutex> lock(active_tasks_mutex_);
            active_tasks_.insert(task);
        }
        task->resume();
        if (task->done()) {
            cleanup_task(task);
            delete task;
        }
    }
}
#endif

// ---------------------------------------------------------------------------
// Read a complete HTTP/1.1 request from socket
// ---------------------------------------------------------------------------

Task<std::optional<request>> server::read_request(tcp::socket& sock) {
    std::string buffer;
    char read_buf[8192];

    // Read until we have a complete request
    for (int attempts = 0; attempts < 1000; ++attempts) {
        // Try to parse what we have
        auto result = http1_codec::parse_request(buffer);
        if (result.has_value()) {
            co_return result->first;
        }

        // Read more data
        auto n = co_await sock.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) {
            co_return std::nullopt; // Connection closed or error
        }

        buffer.append(read_buf, static_cast<size_t>(n));
    }

    co_return std::nullopt; // Too many reads
}

// ---------------------------------------------------------------------------
// Dispatch request to handler
// ---------------------------------------------------------------------------

Task<response> server::dispatch(const request& req) {
    // Match route
    for (auto& r : routes_) {
        if (r.m == req.method && r.path == req.path) {
            co_return co_await r.handler(req);
        }
    }

    // Default handler
    co_return co_await default_handler_(req);
}

// ---------------------------------------------------------------------------
// Write response to socket
// ---------------------------------------------------------------------------

Task<bool> server::write_response(tcp::socket& sock, const response& resp) {
    std::string data = http1_codec::serialize(resp);

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        auto n = co_await sock.async_write_some(
            const_buffer(data.data() + total_sent, data.size() - total_sent));
        if (n <= 0) co_return false;
        total_sent += static_cast<size_t>(n);
    }

    co_return true;
}

// ---------------------------------------------------------------------------
// Handle a single connection (keep-alive loop)
// ---------------------------------------------------------------------------

Task<void> server::handle_connection(tcp::socket sock) {
    while (running_) {
        // Read request
        auto req = co_await read_request(sock);
        if (!req.has_value()) break;

        // Check for WebSocket upgrade
        if (ws::is_websocket_upgrade(*req)) {
            auto* ws_handler = find_ws_handler(req->path);
            if (ws_handler) {
                co_await handle_websocket(sock, *req, *ws_handler);
                co_return;  // WebSocket connection is done
            }
            // No matching WS route — fall through to HTTP dispatch (will likely 404)
        }

        bool keep_alive = req->hdrs.is_keep_alive() && req->ver == version::HTTP_11;

        // Dispatch to handler
        response resp = co_await dispatch(*req);

        // Set Connection header
        if (keep_alive) {
            resp.hdrs.set("Connection", "keep-alive");
        } else {
            resp.hdrs.set("Connection", "close");
        }

        // Ensure Content-Length (always set, even for empty bodies)
        if (!resp.hdrs.contains("Content-Length")) {
            resp.hdrs.set("Content-Length", std::to_string(resp.bd.size()));
        }

        // Write response
        bool ok = co_await write_response(sock, resp);
        if (!ok) break;

        if (!keep_alive) break;
    }

    sock.close();
}

// ---------------------------------------------------------------------------
// WebSocket support
// ---------------------------------------------------------------------------

ws::ws_handler_fn* server::find_ws_handler(const std::string& path) {
    for (auto& r : ws_routes_) {
        if (r.path == path) {
            return &r.handler;
        }
    }
    return nullptr;
}

Task<void> server::handle_websocket(tcp::socket& sock, const request& upgrade_req,
                                     ws::ws_handler_fn handler) {
    // 1. Send 101 Switching Protocols response
    std::string handshake = ws::build_handshake_response(upgrade_req);
    size_t total_sent = 0;
    while (total_sent < handshake.size()) {
        auto n = co_await sock.async_write_some(
            const_buffer(handshake.data() + total_sent, handshake.size() - total_sent));
        if (n <= 0) {
            sock.close();
            co_return;
        }
        total_sent += static_cast<size_t>(n);
    }

    // 2. Create websocket_connection and invoke user handler
    ws::websocket_connection ws_conn(sock);
    co_await handler(ws_conn);

    // 3. Close connection
    sock.close();
}

#ifdef ASYNC_NET_HAS_SSL
Task<void> server::handle_tls_connection(tcp::socket sock, ssl::context& ssl_ctx) {
    ssl::stream stream(sock, ssl_ctx, true);
    auto hs = co_await stream.async_handshake();
    if (hs <= 0) {
        sock.close();
        co_return;
    }

    // Reuse the same keep-alive loop logic via SSL read/write
    std::string buffer;
    char read_buf[8192];

    while (running_) {
        buffer.clear();
        std::optional<request> req_opt;

        for (int attempts = 0; attempts < 1000; ++attempts) {
            auto result = http1_codec::parse_request(buffer);
            if (result.has_value()) {
                req_opt = std::move(result->first);
                break;
            }
            auto n = co_await stream.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
            if (n <= 0) break;
            buffer.append(read_buf, static_cast<size_t>(n));
        }

        if (!req_opt.has_value()) break;

        bool keep_alive = req_opt->hdrs.is_keep_alive() && req_opt->ver == version::HTTP_11;
        response resp = co_await dispatch(*req_opt);

        if (keep_alive) resp.hdrs.set("Connection", "keep-alive");
        else resp.hdrs.set("Connection", "close");

        if (!resp.bd.empty() && !resp.hdrs.contains("Content-Length")) {
            resp.hdrs.set("Content-Length", std::to_string(resp.bd.size()));
        }

        std::string data = http1_codec::serialize(resp);
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            auto n = co_await stream.async_write_some(
                const_buffer(data.data() + total_sent, data.size() - total_sent));
            if (n <= 0) goto done;
            total_sent += static_cast<size_t>(n);
        }

        if (!keep_alive) break;
    }

done:
    co_await stream.async_shutdown();
    sock.close();
}

Task<void> server::handle_h2_connection(tcp::socket sock, ssl::context& ssl_ctx) {
    ssl::stream ssl_stream(sock, ssl_ctx, true);
    auto hs = co_await ssl_stream.async_handshake();
    if (hs <= 0) {
        sock.close();
        co_return;
    }

    // Check ALPN selection
    std::string alpn = ssl_stream.alpn_selected();

    if (alpn == "h2") {
        // HTTP/2 mode
        http2_session session(http2_session::mode::server);

        // Set up request handler that dispatches to our routes (synchronous)
        session.set_request_handler([this](const request& req) -> response {
            for (auto& r : routes_) {
                if (r.m == req.method && r.path == req.path) {
                    auto task = r.handler(req);
                    task.resume();
                    if (task.done()) {
                        return task.handle().promise().result();
                    }
                    return response_internal_error("Handler did not complete synchronously");
                }
            }
            auto task = default_handler_(req);
            task.resume();
            if (task.done()) {
                return task.handle().promise().result();
            }
            return response_not_found();
        });

        // Wire up push provider if set
        if (push_provider_) {
            session.set_push_provider(push_provider_);
        }

        // Send initial SETTINGS
        auto send_fn = [&ssl_stream](const uint8_t* data, size_t len) -> Task<ssize_t> {
            co_return co_await ssl_stream.async_write_some(const_buffer(data, len));
        };
        co_await session.flush(send_fn);

        // Read and validate client connection preface (24 bytes)
        {
            static constexpr const char* PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            static constexpr size_t PREFACE_LEN = 24;
            char preface_buf[256];
            size_t preface_read = 0;
            while (preface_read < PREFACE_LEN) {
                auto n = co_await ssl_stream.async_read_some(
                    mutable_buffer(preface_buf + preface_read, sizeof(preface_buf) - preface_read));
                if (n <= 0) break;
                preface_read += static_cast<size_t>(n);
            }
            if (preface_read < PREFACE_LEN ||
                std::memcmp(preface_buf, PREFACE, PREFACE_LEN) != 0) {
                std::cerr << "[h2] Invalid client connection preface" << std::endl;
                co_await ssl_stream.async_shutdown();
                sock.close();
                co_return;
            }
            // Feed any extra bytes after the preface to the session
            if (preface_read > PREFACE_LEN) {
                session.feed(reinterpret_cast<const uint8_t*>(preface_buf + PREFACE_LEN),
                             preface_read - PREFACE_LEN);
            }
        }

        // Flush any output generated from preface processing
        co_await session.flush(send_fn);

        // Read loop: feed bytes, flush output
        char read_buf[16384];
        while (session.is_alive() && running_) {
            auto n = co_await ssl_stream.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
            if (n <= 0) break;

            auto consumed = session.feed(reinterpret_cast<const uint8_t*>(read_buf),
                                          static_cast<size_t>(n));
            if (consumed < 0) break;

            bool ok = co_await session.flush(send_fn);
            if (!ok) break;
        }

        co_await ssl_stream.async_shutdown();
    } else {
        // Fallback to HTTP/1.1 over TLS
        std::string buffer;
        char buf[8192];

        while (running_) {
            buffer.clear();
            std::optional<request> req_opt;

            for (int attempts = 0; attempts < 1000; ++attempts) {
                auto result = http1_codec::parse_request(buffer);
                if (result.has_value()) {
                    req_opt = std::move(result->first);
                    break;
                }
                auto n = co_await ssl_stream.async_read_some(mutable_buffer(buf, sizeof(buf)));
                if (n <= 0) break;
                buffer.append(buf, static_cast<size_t>(n));
            }

            if (!req_opt.has_value()) break;

            bool keep_alive = req_opt->hdrs.is_keep_alive() && req_opt->ver == version::HTTP_11;
            response resp = co_await dispatch(*req_opt);

            if (keep_alive) resp.hdrs.set("Connection", "keep-alive");
            else resp.hdrs.set("Connection", "close");

            if (!resp.bd.empty() && !resp.hdrs.contains("Content-Length")) {
                resp.hdrs.set("Content-Length", std::to_string(resp.bd.size()));
            }

            std::string data = http1_codec::serialize(resp);
            size_t total_sent = 0;
            while (total_sent < data.size()) {
                auto n = co_await ssl_stream.async_write_some(
                    const_buffer(data.data() + total_sent, data.size() - total_sent));
                if (n <= 0) goto h2_done;
                total_sent += static_cast<size_t>(n);
            }

            if (!keep_alive) break;
        }

h2_done:
        co_await ssl_stream.async_shutdown();
    }

    sock.close();
}
#endif

// ---------------------------------------------------------------------------
// serve_h3() — HTTP/3 over QUIC/UDP
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_HTTP3
Task<void> server::serve_h3(const std::string& cert_file, const std::string& key_file) {
    uint16_t port = port_;

    udp::socket udp_sock(ctx_);
    udp::endpoint ep(port);
    if (!udp_sock.bind(ep)) {
        std::cerr << "[http::server] Failed to bind UDP port " << port << std::endl;
        co_return;
    }

    std::cout << "[http::server] HTTP/3 listening on UDP port " << port << std::endl;

    http3_session::config cfg;
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    cfg.max_streams = 100;

    // Local address for QUIC path validation
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connection map: remote endpoint → session
    struct conn_state {
        std::unique_ptr<http3_session> session;
        udp::endpoint remote;
    };
    std::unordered_map<std::string, conn_state> connections;

    // Build a key from endpoint
    auto ep_key = [](const udp::endpoint& e) -> std::string {
        return e.address() + ":" + std::to_string(e.port());
    };

    // Synchronous request handler that dispatches to our coroutine routes
    auto make_request_handler = [this](const request& req) -> response {
        for (auto& r : routes_) {
            if (r.m == req.method && r.path == req.path) {
                auto task = r.handler(req);
                task.resume();
                if (task.done()) {
                    return task.handle().promise().result();
                }
                return response_internal_error("Handler did not complete synchronously");
            }
        }
        auto task = default_handler_(req);
        task.resume();
        if (task.done()) {
            return task.handle().promise().result();
        }
        return response_not_found();
    };

    char buf[4096];
    udp::endpoint from;

    while (running_) {
        auto n = co_await udp_sock.async_receive_from(
            mutable_buffer(buf, sizeof(buf)), from);
        if (n <= 0) continue;

        std::string key = ep_key(from);

        // Evict dead sessions periodically
        for (auto it = connections.begin(); it != connections.end(); ) {
            if (it->second.session && !it->second.session->is_alive()) {
                it = connections.erase(it);
            } else {
                ++it;
            }
        }

        auto& cs = connections[key];

        if (!cs.session) {
            cs.session = std::make_unique<http3_session>(
                http3_session::mode::server, cfg);
            cs.remote = from;

            cs.session->set_request_handler(make_request_handler);

            if (push_provider_) {
                cs.session->set_push_provider(push_provider_);
            }

            if (!cs.session->init_server_from_packet(
                    reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                    reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                    from.sockaddr_ptr(), from.size())) {
                std::cerr << "[http::server] H3 init_server_from_packet failed" << std::endl;
                cs.session.reset();
                continue;
            }

            // Feed the initial packet to the newly created connection
            // (init_server_from_packet only creates the ngtcp2 conn,
            //  it does NOT process the packet data)
            cs.session->feed_packet(
                reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                from.sockaddr_ptr(), from.size());
        } else {
            cs.session->feed_packet(
                reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n),
                reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                from.sockaddr_ptr(), from.size());
        }

        // Send output packets (use synchronous send_to to avoid blocking the loop)
        auto pkts = cs.session->get_pending_packets();
        for (auto& pkt : pkts) {
            if (!pkt.empty()) {
                udp_sock.send_to(
                    const_buffer(pkt.data(), pkt.size()), from);
            }
        }

        cs.session->handle_expiry();
    }

    udp_sock.close();
}
#endif

// ---------------------------------------------------------------------------
// Multi-threaded serving
// ---------------------------------------------------------------------------

void server::serve_mt(unsigned int num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
    }

    if (num_threads <= 1) {
        // Single-threaded: just serve normally
        auto task = serve();
        task.resume();
        ctx_.run();
        return;
    }

    if (ctx_.backend().supports_concurrent_poll()) {
        // epoll / kqueue / IOCP: single server, N threads share the io_context.
        // Backend distributes events across threads (poll() is thread-safe).
        std::cout << "[serve_mt] Concurrent-poll (" << ctx_.backend().name()
                  << "): " << num_threads << " threads\n";
        auto task = serve();
        task.resume();
        ctx_.run_mt(num_threads);
    } else {
        // io_uring (or other non-concurrent backends):
        // SO_REUSEPORT — each worker has its own io_context + server.
        // Delegates to run_mt() with a worker_factory callback.
        std::cout << "[serve_mt] SO_REUSEPORT (" << ctx_.backend().name()
                  << "): " << num_threads << " workers\n";
        ctx_.run_mt(num_threads, [this](io_context& wctx) {
            // Each worker creates its own io_context + server
            server wsrv(wctx, port_, addr_.c_str(), /*reuse_port=*/true);
            // Copy routes and handlers from this server
            wsrv.routes_ = this->routes_;
            wsrv.ws_routes_ = this->ws_routes_;
            wsrv.default_handler_ = this->default_handler_;
            wsrv.push_provider_ = this->push_provider_;
            auto task = wsrv.serve();
            task.resume();
            wctx.run();
        });
    }
}

} // namespace async_net::http
