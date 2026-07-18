// HTTP/2 Server Example — serves both H2 and HTTP/1.1 over TLS
// Usage: ./h2_server [port] [cert.pem] [key.pem]
// Test: curl --http2 -k https://localhost:8443/

#include <async_net/http/server.hpp>
#include <iostream>

using namespace async_net;
using namespace async_net::http;

Task<void> run_server(io_context& ctx, uint16_t port,
                      const char* cert_path, const char* key_path) {
    ssl::context ssl_ctx("tls_server");
    if (!ssl_ctx.use_certificate_file(cert_path)) {
        std::cerr << "Failed to load certificate: " << cert_path << std::endl;
        co_return;
    }
    if (!ssl_ctx.use_private_key_file(key_path)) {
        std::cerr << "Failed to load key: " << key_path << std::endl;
        co_return;
    }

    server srv(ctx, port);

    srv.route(method::GET, "/", [](const request&) -> Task<response> {
        co_return response_ok("Hello from async_net H2 server!");
    });

    srv.route(method::GET, "/json", [](const request&) -> Task<response> {
        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "application/json")
            .body(R"({"protocol": "h2", "message": "Hello HTTP/2!"})")
            .build();
    });

    srv.route(method::POST, "/echo", [](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", req.hdrs.get("Content-Type").value_or("text/plain"))
            .body(req.bd.data())
            .build();
    });

    srv.default_handler([](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::not_found())
            .body("404 Not Found: " + req.path)
            .build();
    });

    std::cout << "[h2_server] Starting on port " << port << std::endl;
    co_await srv.serve_h2(ssl_ctx);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8443;
    const char* cert = "cert.pem";
    const char* key = "key.pem";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) cert = argv[2];
    if (argc > 3) key = argv[3];

    try {
        io_context ctx;
        auto task = run_server(ctx, port, cert, key);
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
