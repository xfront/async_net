// HTTP Server Example — simple web server with routes
// Usage: ./http_server [port]

#include <async_net/http/server.hpp>
#include <iostream>

using namespace async_net;
using namespace async_net::http;

Task<void> run_server(io_context& ctx, uint16_t port) {
    server srv(ctx, port);

    // GET /
    srv.route(method::GET, "/", [](const request&) -> Task<response> {
        co_return response_ok("Hello from async_net HTTP server!");
    });

    // GET /hello/:name (simple path matching)
    srv.route(method::GET, "/hello", [](const request& req) -> Task<response> {
        auto name = req.hdrs.get("X-Name").value_or("World");
        co_return response_ok("Hello, " + name + "!");
    });

    // POST /echo
    srv.route(method::POST, "/echo", [](const request& req) -> Task<response> {
        response resp = response_make()
            .status(status_code::ok())
            .header("Content-Type", req.hdrs.get("Content-Type").value_or("text/plain"))
            .body(req.bd.data())
            .build();
        co_return resp;
    });

    // GET /json
    srv.route(method::GET, "/json", [](const request&) -> Task<response> {
        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "application/json")
            .body(R"({"message": "Hello, JSON!", "status": "ok"})")
            .build();
    });

    // GET /headers
    srv.route(method::GET, "/headers", [](const request& req) -> Task<response> {
        std::string body_text = "Request Headers:\n";
        for (auto& [k, v] : req.hdrs) {
            body_text += "  " + k + ": " + v + "\n";
        }
        co_return response_ok(body_text);
    });

    // Default 404
    srv.default_handler([](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::not_found())
            .body("404 Not Found: " + req.path)
            .build();
    });

    std::cout << "[http_server] Starting on port " << port << std::endl;
    co_await srv.serve();
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    try {
        io_context ctx;
        auto task = run_server(ctx, port);
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
