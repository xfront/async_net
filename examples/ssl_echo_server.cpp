// SSL Echo Server — accepts TLS connections and echoes data back.
// Usage: ./ssl_echo_server [port] [cert.pem] [key.pem]
//
// Generate self-signed certs for testing:
//   openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
//               -days 365 -nodes -subj "/CN=localhost"

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <async_net/io/ssl.hpp>
#include <iostream>
#include <cstring>
#include <unordered_set>

using namespace async_net;

static std::unordered_set<Task<void>*> active_tasks;

Task<void> handle_client(tcp::socket sock, ssl::context& ctx) {
    ssl::stream ssl_sock(sock, ctx, /*is_server=*/true);
    int client_num = sock.native_handle();

    auto ret = co_await ssl_sock.async_handshake();
    if (ret <= 0) {
        std::cerr << "[Server] SSL handshake failed for client " << client_num << std::endl;
        co_return;
    }
    std::cout << "[Server] Client " << client_num << " connected (TLS)" << std::endl;

    char buf[4096];
    while (true) {
        auto n = co_await ssl_sock.async_read_some(mutable_buffer(buf, sizeof(buf)));
        if (n <= 0) {
            std::cout << "[Server] Client " << client_num << " disconnected" << std::endl;
            break;
        }

        std::cout << "[Server] Received " << n << " bytes from client " << client_num << std::endl;

        auto written = co_await ssl_sock.async_write_some(const_buffer(buf, n));
        if (written <= 0) {
            std::cerr << "[Server] Write failed for client " << client_num << std::endl;
            break;
        }
        std::cout << "[Server] Echoed " << written << " bytes to client " << client_num << std::endl;
    }

    co_await ssl_sock.async_shutdown();
}

Task<void> run_server(io_context& ctx, uint16_t port, ssl::context& ssl_ctx) {
    tcp::acceptor acc(ctx, port);
    if (!acc.is_open()) {
        std::cerr << "[Server] Failed to open acceptor on port " << port << std::endl;
        co_return;
    }

    std::cout << "[Server] Listening on port " << port << " (TLS)" << std::endl;

    while (true) {
        auto sock = co_await acc.async_accept();
        if (!sock.is_open()) {
            std::cerr << "[Server] Accept failed" << std::endl;
            co_return;
        }

        auto* task = new Task<void>(handle_client(std::move(sock), ssl_ctx));
        active_tasks.insert(task);
        task->resume();
        if (task->done()) {
            active_tasks.erase(task);
            delete task;
        }
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 8443;
    const char* cert_file = "server_cert.pem";
    const char* key_file = "server_key.pem";

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) cert_file = argv[2];
    if (argc > 3) key_file = argv[3];

    try {
        io_context ctx;

        ssl::context ssl_ctx("tls_server");
        if (!ssl_ctx.use_certificate_file(cert_file)) {
            std::cerr << "[Server] Failed to load certificate: " << cert_file << std::endl;
            return 1;
        }
        if (!ssl_ctx.use_private_key_file(key_file)) {
            std::cerr << "[Server] Failed to load private key: " << key_file << std::endl;
            return 1;
        }

        std::cout << "[Server] Using backend: " << ctx.backend().name() << std::endl;

        auto* server_task = new Task<void>(run_server(ctx, port, ssl_ctx));
        server_task->resume();
        if (server_task->done()) { delete server_task; return 0; }
        ctx.run();
        delete server_task;
    } catch (const std::exception& e) {
        std::cerr << "[Server] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
