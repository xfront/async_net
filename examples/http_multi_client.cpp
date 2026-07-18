// Multi-Protocol HTTP Client — unified URL with automatic protocol selection
//
// A single URL serves all three protocols (same as browsers):
//   https://host:port/path  → TLS + ALPN (negotiates H2 or HTTP/1.1)
//   h3://host:port/path     → HTTP/3 (QUIC/UDP, for testing)
//
// With --http3 flag:
//   First request via HTTPS → receives Alt-Svc header → retries via H3
//
// Usage: ./http_multi_client [--http3] <url> [method] [body]
//
// Examples:
//   ./http_multi_client https://127.0.0.1:8443/
//   ./http_multi_client https://127.0.0.1:8443/json
//   ./http_multi_client https://127.0.0.1:8443/echo POST "hello"
//   ./http_multi_client --http3 https://127.0.0.1:8443/     # auto-upgrade to H3
//   ./http_multi_client h3://127.0.0.1:443/                 # direct H3

#include <async_net/http/client.hpp>
#include <async_net/http/http2_session.hpp>
#include <async_net/net/tcp.hpp>
#include <async_net/net/udp.hpp>
#include <async_net/io/io_context.hpp>

#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#endif

#include <iostream>
#include <optional>

using namespace async_net;
using namespace async_net::http;

// ---------------------------------------------------------------------------
// Print a response
// ---------------------------------------------------------------------------

static void print_response(const response& resp, const std::string& protocol_label) {
    std::cout << "Protocol: " << protocol_label << std::endl;
    std::cout << "Status:   " << resp.status.as_int() << " "
              << resp.status.reason_phrase() << std::endl;

    for (auto& [k, v] : resp.hdrs) {
        std::cout << "  " << k << ": " << v << std::endl;
    }

    if (!resp.bd.empty()) {
        std::cout << std::endl;
        std::cout << resp.bd.data() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Parse Alt-Svc header to extract H3 port
//   Alt-Svc: h3=":443"; ma=86400
// ---------------------------------------------------------------------------

static std::optional<uint16_t> parse_alt_svc_h3_port(const response& resp) {
    auto alt_svc = resp.hdrs.get("Alt-Svc");
    if (!alt_svc.has_value()) return std::nullopt;

    // Look for h3=":PORT"
    const std::string& val = *alt_svc;
    auto pos = val.find("h3=\"");
    if (pos == std::string::npos) return std::nullopt;

    pos = val.find(':', pos + 4);
    if (pos == std::string::npos) return std::nullopt;
    pos++; // skip ':'

    int port = 0;
    while (pos < val.size() && val[pos] >= '0' && val[pos] <= '9') {
        port = port * 10 + (val[pos] - '0');
        pos++;
    }

    return (port > 0 && port < 65536) ? std::optional<uint16_t>(static_cast<uint16_t>(port))
                                       : std::nullopt;
}

// ---------------------------------------------------------------------------
// Fetch via TLS (HTTP/1.1 or HTTP/2 via ALPN negotiation)
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_SSL
static Task<response> fetch_https(io_context& ctx, const uri& u,
                                  method m, const std::string& body_text) {
    ssl::context ssl_ctx("tls_client");
    ssl_ctx.set_verify_peer(false); // self-signed certs

    // Request both h2 and http/1.1 via ALPN
    ssl_ctx.set_alpn_protos({"h2", "http/1.1"});

    // Connect TCP
    tcp::socket sock(ctx);
    auto ret = co_await sock.async_connect(u.host().c_str(), u.port());
    if (ret < 0) {
        response resp;
        resp.status = status_code::internal_error();
        resp.bd = body("Connection failed");
        co_return resp;
    }

    // TLS handshake
    ssl::stream ssl_stream(sock, ssl_ctx, false);
    auto hs = co_await ssl_stream.async_handshake();
    if (hs <= 0) {
        response resp;
        resp.status = status_code::internal_error();
        resp.bd = body("TLS handshake failed");
        co_return resp;
    }

    std::string alpn = ssl_stream.alpn_selected();

    if (alpn == "h2") {
        // ---- HTTP/2 mode ----
        http2_session session(http2_session::mode::client);

        auto send_fn = [&ssl_stream](const uint8_t* data, size_t len) -> Task<ssize_t> {
            co_return co_await ssl_stream.async_write_some(const_buffer(data, len));
        };

        // Send HTTP/2 connection preface
        static const char* H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        size_t preface_sent = 0;
        while (preface_sent < 24) {
            auto n = co_await ssl_stream.async_write_some(
                const_buffer(H2_PREFACE + preface_sent, 24 - preface_sent));
            if (n <= 0) {
                co_return response_make().status(status_code::internal_error())
                    .body("Failed to send H2 preface").build();
            }
            preface_sent += static_cast<size_t>(n);
        }

        // Send client SETTINGS
        co_await session.flush(send_fn);

        // Submit request
        request req = request_make()
            .method_(m)
            .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
            .header(":authority", u.host())
            .header("user-agent", "async_net/1.0")
            .body(body_text)
            .build();
        req.ver = version::HTTP_2;

        auto promise = std::make_shared<http2_session::response_promise>();
        session.submit_request(req, promise);
        co_await session.flush(send_fn);

        // Read loop until response completes
        char read_buf[16384];
        while (!promise->complete && session.is_alive()) {
            auto n = co_await ssl_stream.async_read_some(
                mutable_buffer(read_buf, sizeof(read_buf)));
            if (n <= 0) break;

            session.feed(reinterpret_cast<const uint8_t*>(read_buf),
                         static_cast<size_t>(n));
            co_await session.flush(send_fn);
        }

        co_await ssl_stream.async_shutdown();
        sock.close();

        if (promise->complete) {
            response resp = std::move(promise->resp);
            resp.ver = version::HTTP_2;
            co_return resp;
        }

        co_return response_make().status(status_code::internal_error())
            .body("H2 request incomplete").build();
    } else {
        // ---- HTTP/1.1 over TLS ----
        request req = request_make()
            .method_(m)
            .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
            .header("Host", u.host())
            .header("User-Agent", "async_net/1.0")
            .header("Connection", "close")
            .body(body_text)
            .build();

        std::string raw;
        raw += std::string(to_string(m)) + " " + req.path + " HTTP/1.1\r\n";
        for (auto& [k, v] : req.hdrs) {
            raw += k + ": " + v + "\r\n";
        }
        if (!body_text.empty()) {
            raw += "Content-Length: " + std::to_string(body_text.size()) + "\r\n";
        }
        raw += "\r\n";
        raw += body_text;

        size_t total = 0;
        while (total < raw.size()) {
            auto n = co_await ssl_stream.async_write_some(
                const_buffer(raw.data() + total, raw.size() - total));
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }

        // Read response
        std::string resp_buf;
        char rbuf[8192];
        while (true) {
            auto n = co_await ssl_stream.async_read_some(
                mutable_buffer(rbuf, sizeof(rbuf)));
            if (n <= 0) break;
            resp_buf.append(rbuf, static_cast<size_t>(n));
        }

        co_await ssl_stream.async_shutdown();
        sock.close();

        // Parse HTTP/1.1 response (extract headers for Alt-Svc)
        response resp;
        resp.status = status_code::ok();
        resp.ver = version::HTTP_11;

        // Simple header parsing to capture Alt-Svc
        auto header_end = resp_buf.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            std::string headers_str = resp_buf.substr(0, header_end);
            std::string body_str = resp_buf.substr(header_end + 4);

            // Parse status line
            auto status_pos = headers_str.find(' ');
            if (status_pos != std::string::npos) {
                int code = std::atoi(headers_str.c_str() + status_pos + 1);
                if (code > 0) resp.status = status_code(code);
            }

            // Parse headers
            size_t pos = headers_str.find("\r\n") + 2;
            while (pos < headers_str.size()) {
                auto line_end = headers_str.find("\r\n", pos);
                if (line_end == std::string::npos) break;
                std::string line = headers_str.substr(pos, line_end - pos);
                auto colon = line.find(": ");
                if (colon != std::string::npos) {
                    resp.hdrs.set(line.substr(0, colon), line.substr(colon + 2));
                }
                pos = line_end + 2;
            }

            resp.bd = body(body_str);
        } else {
            resp.bd = body(resp_buf);
        }

        co_return resp;
    }
}
#endif

// ---------------------------------------------------------------------------
// Fetch via HTTP/3 (QUIC/UDP)
// ---------------------------------------------------------------------------

#ifdef ASYNC_NET_HAS_HTTP3
static Task<response> fetch_h3(io_context& ctx, const uri& u,
                               method m, const std::string& body_text) {
    udp::socket sock(ctx);
    udp::endpoint remote_ep(u.port(), u.host().c_str());

    // Set receive timeout to prevent hanging forever
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    http::http3_session::config cfg;
    cfg.max_streams = 10;
    cfg.host = u.host();
    cfg.port = u.port();

    auto session = std::make_unique<http::http3_session>(
        http::http3_session::mode::client, cfg);

    if (!session->is_alive()) {
        co_return response_make().status(status_code::internal_error())
            .body("H3 session init failed").build();
    }

    // Prepare path addresses for feed_packet (needed by ngtcp2 path validation)
    struct sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0;

    // Send initial Client Hello
    auto send_fn = [&sock, &remote_ep](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return sock.send_to(const_buffer(data, len), remote_ep);
    };
    co_await session->flush(send_fn);

    // Build and submit the H3 request (queued until handshake completes)
    request req = request_make()
        .method_(m)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header(":authority", u.host())
        .body(body_text)
        .build();
    req.ver = version::HTTP_3;

    auto promise = std::make_shared<http3_session::response_promise>();
    auto sid = session->submit_request(req, promise);

    // Helper: send all pending packets as individual UDP datagrams
    auto send_pending = [&]() {
        auto pkts = session->get_pending_packets();
        for (auto& pkt : pkts) {
            if (!pkt.empty()) {
                sock.send_to(const_buffer(pkt.data(), pkt.size()), remote_ep);
            }
        }
    };

    // Flush any queued request data
    co_await session->flush(send_fn);

    // Read loop with timeout
    char buf[4096];
    udp::endpoint from;
    int max_rounds = 200;
    int idle_rounds = 0;

    while (!promise->complete && session->is_alive() && max_rounds-- > 0) {
        auto n = co_await sock.async_receive_from(
            mutable_buffer(buf, sizeof(buf)), from);
        if (n <= 0) {
            idle_rounds++;
            if (idle_rounds > 10) break;
            session->handle_expiry();
            send_pending();
            continue;
        }
        idle_rounds = 0;

        session->feed_packet(reinterpret_cast<const uint8_t*>(buf),
                             static_cast<size_t>(n),
                             reinterpret_cast<const struct sockaddr*>(&local_addr), sizeof(local_addr),
                             remote_ep.sockaddr_ptr(), remote_ep.size());

        send_pending();
        session->handle_expiry();
    }

    sock.close();

    if (promise->complete && !promise->error) {
        co_return std::move(promise->resp);
    }

    co_return response_make().status(status_code::internal_error())
        .body("H3 request timed out or incomplete").build();
}
#endif

// ---------------------------------------------------------------------------
// Main fetch dispatcher
// ---------------------------------------------------------------------------

static Task<void> do_fetch(io_context& ctx, const std::string& url,
                           const std::string& method_str, const std::string& body_text,
                           bool try_h3) {
    uri u(url);
    auto m = parse_method(method_str).value_or(method::GET);

    std::cout << "URL:    " << url << std::endl;
    std::cout << "Method: " << to_string(m) << std::endl;
    if (!body_text.empty()) {
        std::cout << "Body:   " << body_text << std::endl;
    }
    if (try_h3) {
        std::cout << "Mode:   H3 auto-upgrade via Alt-Svc" << std::endl;
    }
    std::cout << std::endl;

    std::string scheme = u.scheme();

#ifdef ASYNC_NET_HAS_SSL
    if (scheme == "https") {
        // TLS with H2 + H1.1 ALPN negotiation
        auto resp = co_await fetch_https(ctx, u, m, body_text);

        std::string label;
        if (resp.ver == version::HTTP_2) label = "HTTP/2 (TLS + ALPN)";
        else label = "HTTP/1.1 (TLS)";

        print_response(resp, label);

        // Check for Alt-Svc H3 upgrade
        if (try_h3) {
            auto h3_port = parse_alt_svc_h3_port(resp);
            if (h3_port.has_value()) {
#ifdef ASYNC_NET_HAS_HTTP3
                std::cout << "\n--- Alt-Svc detected: upgrading to H3 on port "
                          << *h3_port << " ---\n" << std::endl;

                uri h3_uri = u;
                h3_uri.parse("h3://" + u.host() + ":" + std::to_string(*h3_port) + u.path());

                auto h3_resp = co_await fetch_h3(ctx, h3_uri, m, body_text);
                print_response(h3_resp, "HTTP/3 (QUIC/UDP, via Alt-Svc upgrade)");
#else
                std::cout << "\n--- Alt-Svc offers H3 on port " << *h3_port
                          << " (H3 not compiled in) ---" << std::endl;
#endif
            } else {
                std::cout << "\n--- No Alt-Svc header found (server may not advertise H3) ---"
                          << std::endl;
            }
        }

        ctx.stop();
        co_return;
    }
#endif

#ifdef ASYNC_NET_HAS_HTTP3
    if (scheme == "h3") {
        auto resp = co_await fetch_h3(ctx, u, m, body_text);
        print_response(resp, "HTTP/3 (QUIC/UDP)");
        ctx.stop();
        co_return;
    }
#endif

    std::cerr << "Unsupported scheme: " << scheme << std::endl;
    std::cerr << "Supported: https://";
#ifdef ASYNC_NET_HAS_HTTP3
    std::cerr << ", h3://";
#endif
    std::cerr << std::endl;

    ctx.stop();
}

int main(int argc, char* argv[]) {
    bool try_h3 = false;
    int url_arg = 1;

    // Check for --http3 flag
    if (argc >= 2 && std::string(argv[1]) == "--http3") {
        try_h3 = true;
        url_arg = 2;
    }

    if (argc < url_arg + 1) {
        std::cerr << "Usage: " << argv[0] << " [--http3] <url> [method] [body]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "A single URL supports all protocols (like browsers):" << std::endl;
        std::cerr << "  https://host:port/path  - TLS + ALPN (HTTP/2 or HTTP/1.1)" << std::endl;
#ifdef ASYNC_NET_HAS_HTTP3
        std::cerr << "  h3://host:port/path     - HTTP/3 (QUIC/UDP, for testing)" << std::endl;
#endif
        std::cerr << std::endl;
        std::cerr << "Flags:" << std::endl;
        std::cerr << "  --http3  First request via HTTPS, read Alt-Svc, then retry via H3" << std::endl;
        return 1;
    }

    std::string url = argv[url_arg];
    std::string method_str = (argc > url_arg + 1) ? argv[url_arg + 1] : "GET";
    std::string body_text = (argc > url_arg + 2) ? argv[url_arg + 2] : "";

    try {
        io_context ctx;
        auto task = do_fetch(ctx, url, method_str, body_text, try_h3);
        task.resume();
        if (!task.done()) {
            ctx.run();
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
