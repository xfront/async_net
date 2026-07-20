// HTTP Client — unified multi-protocol client (HTTP/1.1 + HTTP/2 + HTTP/3)
//
// A single URL automatically selects the right protocol:
//   http://host:port/path   → HTTP/1.1 (plain TCP)
//   https://host:port/path  → TLS + ALPN (HTTP/2 or HTTP/1.1)
//   h3://host:port/path     → HTTP/3 (QUIC/UDP)
//
// With --http3 flag:
//   First request via HTTPS → receives Alt-Svc header → retries via H3
//
// Usage: ./http_client [--http3] <url> [method] [body]
//
// Examples:
//   ./http_client http://localhost:8080/
//   ./http_client https://localhost:8443/json
//   ./http_client h3://localhost:4433/
//   ./http_client https://localhost:8443/echo POST "hello"
//   ./http_client --http3 https://localhost:8443/     # auto-upgrade to H3

#include <async_net/http/client.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>

#include <iostream>

using namespace async_net;
using namespace async_net::http;

// ---------------------------------------------------------------------------
// Parse Alt-Svc header to extract H3 port
//   Alt-Svc: h3=":443"; ma=86400
// ---------------------------------------------------------------------------

static std::optional<uint16_t> parse_alt_svc_h3_port(const response& resp) {
    auto alt_svc = resp.hdrs.get("Alt-Svc");
    if (!alt_svc.has_value()) return std::nullopt;

    const std::string& val = *alt_svc;

    // Try quoted format: h3=":port"
    auto pos = val.find("h3=\"");
    if (pos == std::string::npos) {
        // Try unquoted format: h3=:port or h3=port
        pos = val.find("h3=");
        if (pos == std::string::npos) return std::nullopt;
        pos += 3;
        if (pos < val.size() && val[pos] == '"') pos++;
        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t')) pos++;
        if (pos < val.size() && val[pos] == ':') pos++;
        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t')) pos++;
    } else {
        pos = val.find(':', pos + 4);
        if (pos == std::string::npos) return std::nullopt;
        pos++;
        while (pos < val.size() && val[pos] == ' ') pos++;
    }

    int port = 0;
    while (pos < val.size() && val[pos] >= '0' && val[pos] <= '9') {
        port = port * 10 + (val[pos] - '0');
        pos++;
    }

    return (port > 0 && port < 65536) ? std::optional<uint16_t>(static_cast<uint16_t>(port))
                                       : std::nullopt;
}

// ---------------------------------------------------------------------------
// Protocol label from version
// ---------------------------------------------------------------------------

static const char* protocol_label(version ver) {
    switch (ver) {
#ifdef ASYNC_NET_HAS_SSL
        case version::HTTP_2: return "HTTP/2 (TLS + ALPN)";
#endif
#ifdef ASYNC_NET_HAS_HTTP3
        case version::HTTP_3: return "HTTP/3 (QUIC/UDP)";
#endif
        default: return "HTTP/1.1";
    }
}

// ---------------------------------------------------------------------------
// Print response
// ---------------------------------------------------------------------------

static void print_response(const response& resp) {
    std::cout << "Protocol: " << protocol_label(resp.ver) << std::endl;
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
// Send request using the unified client class
// ---------------------------------------------------------------------------

static Task<response> do_request(client& cli, const uri& u,
                                  const std::string& method_str,
                                  const std::string& body_text) {
    auto m = parse_method(method_str).value_or(method::GET);

    if (method_str == "GET" || method_str == "get") {
        co_return co_await cli.get(
            u.scheme() + "://" + u.host() + ":" + std::to_string(u.port()) +
            u.path() + (u.query().empty() ? "" : "?" + u.query()));
    }

    request req = request_make()
        .method_(m)
        .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
        .header("Host", u.host())
        .header("User-Agent", "async_net/1.0")
        .header("Connection", "keep-alive")
        .body(body_text)
        .build();

    if (!body_text.empty()) {
        req.hdrs.set("Content-Type", "text/plain");
    }

    co_return co_await cli.send(u.host(), u.port(), std::move(req), u.is_https());
}

// ---------------------------------------------------------------------------
// Main fetch dispatcher
// ---------------------------------------------------------------------------

static Task<void> fetch(io_context& ctx, const std::string& url,
                         const std::string& method_str, const std::string& body_text,
                         bool try_h3_upgrade) {
    uri u(url);

    std::cout << "URL:    " << url << std::endl;
    std::cout << "Method: " << method_str << std::endl;
    if (!body_text.empty()) {
        std::cout << "Body:   " << body_text << std::endl;
    }
    if (try_h3_upgrade) {
        std::cout << "Mode:   H3 auto-upgrade via Alt-Svc" << std::endl;
    }
    std::cout << std::endl;

#ifdef ASYNC_NET_HAS_SSL
    ssl::context ssl_ctx("tls_client");
    ssl_ctx.set_verify_peer(false);
    ssl_ctx.set_alpn_protos({"h2", "http/1.1"});
    client cli(ctx, ssl_ctx);
#else
    client cli(ctx);
#endif

    // Send initial request
    response resp = co_await do_request(cli, u, method_str, body_text);
    print_response(resp);

#ifdef ASYNC_NET_HAS_HTTP3
    // Alt-Svc H3 upgrade
    if (try_h3_upgrade && u.is_https()) {
        auto h3_port = parse_alt_svc_h3_port(resp);
        if (h3_port.has_value()) {
            std::cout << "\n--- Alt-Svc detected: upgrading to H3 on port "
                      << *h3_port << " ---\n" << std::endl;

            std::string h3_url = "h3://" + u.host() + ":" +
                                 std::to_string(*h3_port) + u.path();
            if (!u.query().empty()) h3_url += "?" + u.query();

            response h3_resp = co_await do_request(cli, uri(h3_url), method_str, body_text);
            print_response(h3_resp);
        } else {
            std::cout << "\n--- No Alt-Svc header (server may not advertise H3) ---" << std::endl;
        }
    }
#else
    (void)try_h3_upgrade;
#endif

    ctx.stop();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    bool try_h3 = false;
    int url_arg = 1;

    if (argc >= 2 && std::string(argv[1]) == "--http3") {
        try_h3 = true;
        url_arg = 2;
    }

    if (argc < url_arg + 1) {
        std::cerr << "Usage: " << argv[0] << " [--http3] <url> [method] [body]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "A single URL supports all protocols (like browsers):" << std::endl;
        std::cerr << "  http://host:port/path   - HTTP/1.1 (plain)" << std::endl;
#ifdef ASYNC_NET_HAS_SSL
        std::cerr << "  https://host:port/path  - TLS + ALPN (HTTP/2 or HTTP/1.1)" << std::endl;
#endif
#ifdef ASYNC_NET_HAS_HTTP3
        std::cerr << "  h3://host:port/path     - HTTP/3 (QUIC/UDP)" << std::endl;
#endif
        std::cerr << std::endl;
        std::cerr << "Flags:" << std::endl;
        std::cerr << "  --http3  First HTTPS request, read Alt-Svc, then retry via H3" << std::endl;
        return 1;
    }

    std::string url = argv[url_arg];
    std::string method_str = (argc > url_arg + 1) ? argv[url_arg + 1] : "GET";
    std::string body_text = (argc > url_arg + 2) ? argv[url_arg + 2] : "";

    try {
        io_context ctx;
        auto task = fetch(ctx, url, method_str, body_text, try_h3);
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
