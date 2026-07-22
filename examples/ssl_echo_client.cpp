// SSL Echo Client — connects via TLS and sends/receives messages.
// Usage: ./ssl_echo_client [host] [port] [ca.pem]
//
// For self-signed certs, pass the cert.pem as ca.pem to skip verification.

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <async_net/io/ssl.hpp>
#include <iostream>
#include <cstring>

using namespace async_net;

Task<void> run_client(io_context& ctx, const char* host, uint16_t port, const char* ca_file) {
    tcp::socket sock(ctx);
    if (!sock.is_open()) {
        std::cerr << "[Client] Failed to create socket" << std::endl;
        co_return;
    }

    std::cout << "[Client] Connecting to " << host << ":" << port << std::endl;

    auto ret = co_await sock.async_connect(host, port);
    if (ret < 0) {
        std::cerr << "[Client] Connection failed" << std::endl;
        co_return;
    }

    std::cout << "[Client] TCP connected, starting TLS handshake..." << std::endl;

    ssl::context ssl_ctx("tls_client");
    if (ca_file) {
        ssl_ctx.load_verify_file(ca_file);
    } else {
        ssl_ctx.set_verify_peer(false); // Skip verification for testing
    }

    ssl::stream ssl_sock(sock, ssl_ctx, /*is_server=*/false);
    auto hs = co_await ssl_sock.async_handshake();
    if (hs <= 0) {
        std::cerr << "[Client] SSL handshake failed" << std::endl;
        co_return;
    }

    std::cout << "[Client] TLS handshake successful!" << std::endl;

    // Send some messages
    const char* messages[] = {
        "Hello, TLS world!",
        "C++20 coroutines + SSL are awesome",
        "Goodbye!"
    };

    char buf[4096];
    for (const char* msg : messages) {
        size_t len = std::strlen(msg);
        auto written = co_await ssl_sock.async_write_some(const_buffer(msg, len));
        if (written <= 0) {
            std::cerr << "[Client] Write failed" << std::endl;
            break;
        }

        std::cout << "[Client] Sent: " << msg << std::endl;

        auto n = co_await ssl_sock.async_read_some(mutable_buffer(buf, sizeof(buf)));
        if (n <= 0) {
            std::cerr << "[Client] Read failed" << std::endl;
            break;
        }

        std::cout << "[Client] Received: " << std::string(buf, n) << std::endl;
    }

    co_await ssl_sock.async_shutdown();
    sock.close();
    std::cout << "[Client] Done" << std::endl;
    ctx.stop();
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 8443;
    const char* ca_file = nullptr;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) ca_file = argv[3];

    try {
        io_context ctx;
        std::cout << "[Client] Using backend: " << ctx.backend().name() << std::endl;

        auto client_task = run_client(ctx, host, port, ca_file);
        client_task.resume();
        if (client_task.done()) return 0;
        ctx.run();
    } catch (const std::exception& e) {
        std::cerr << "[Client] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
