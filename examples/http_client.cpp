// HTTP Client Example — fetch URLs
// Usage: ./http_client <url> [method] [body]
// Example: ./http_client http://localhost:8080/
//          ./http_client http://localhost:8080/echo POST "hello world"

#include <async_net/http/client.hpp>
#include <iostream>

using namespace async_net;
using namespace async_net::http;

Task<void> fetch(io_context& ctx, const std::string& url,
                 const std::string& method_str, const std::string& body_text) {
    client cli(ctx);

    std::cout << "[http_client] " << method_str << " " << url << std::endl;

    response resp;
    if (method_str == "GET" || method_str == "get") {
        resp = co_await cli.get(url);
    } else if (method_str == "POST" || method_str == "post") {
        resp = co_await cli.post(url, body_text, "text/plain");
    } else if (method_str == "PUT" || method_str == "put") {
        resp = co_await cli.put(url, body_text, "text/plain");
    } else if (method_str == "DELETE" || method_str == "delete") {
        resp = co_await cli.delete_(url);
    } else {
        // Custom method
        uri u(url);
        auto m = parse_method(method_str).value_or(method::GET);
        request req = request_make()
            .method_(m)
            .path(u.path() + (u.query().empty() ? "" : "?" + u.query()))
            .header("Host", u.host())
            .header("User-Agent", "async_net/1.0")
            .body(body_text)
            .build();
        resp = co_await cli.send(u.host(), u.port(), std::move(req), u.is_https());
    }

    std::cout << "[http_client] " << to_string(resp.ver) << " "
              << resp.status.as_int() << " " << resp.status.reason_phrase() << std::endl;

    for (auto& [k, v] : resp.hdrs) {
        std::cout << "  " << k << ": " << v << std::endl;
    }

    std::cout << std::endl;
    if (!resp.bd.empty()) {
        std::cout << resp.bd.data() << std::endl;
    }

    ctx.stop();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <url> [method] [body]" << std::endl;
        std::cerr << "  method: GET (default), POST, PUT, DELETE" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    std::string method_str = (argc > 2) ? argv[2] : "GET";
    std::string body_text = (argc > 3) ? argv[3] : "";

    try {
        io_context ctx;
        auto task = fetch(ctx, url, method_str, body_text);
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
