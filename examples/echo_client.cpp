#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <async_net/net/buffer.hpp>
#include <async_net/coroutine/task.hpp>
#include <iostream>
#include <cstring>

using namespace async_net;

Task<void> run_client(io_context& ctx, const char* host, uint16_t port) {
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

    std::cout << "[Client] Connected!" << std::endl;

    // Send some messages
    const char* messages[] = {
        "Hello, async world!",
        "C++20 coroutines are awesome",
        "Goodbye!"
    };

    char buf[4096];
    for (const char* msg : messages) {
        size_t len = std::strlen(msg);
        auto written = co_await sock.async_write_some(const_buffer(msg, len));
        if (written <= 0) {
            std::cerr << "[Client] Write failed" << std::endl;
            break;
        }

        std::cout << "[Client] Sent: " << msg << std::endl;

        auto n = co_await sock.async_read_some(mutable_buffer(buf, sizeof(buf)));
        if (n <= 0) {
            std::cerr << "[Client] Read failed" << std::endl;
            break;
        }

        std::cout << "[Client] Received: " << std::string(buf, n) << std::endl;
    }

    sock.close();
    std::cout << "[Client] Done" << std::endl;

    // Stop the io_context after we're done
    ctx.stop();
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 6543;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));

    try {
        io_context ctx;
        std::cout << "[Client] Using backend: " << ctx.backend().name() << std::endl;

        auto client_task = run_client(ctx, host, port);
        client_task.resume();

        ctx.run();
    } catch (const std::exception& e) {
        std::cerr << "[Client] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
