// Broadcast receiver — listens for UDP broadcast messages.
// Usage: ./broadcast_receiver [port]

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/udp.hpp>
#include <cstdio>
#include <cstring>

using namespace async_net;

Task<void> run_receiver(io_context& ctx, uint16_t port) {
    udp::socket sock(ctx);
    if (!sock.is_open()) {
        fprintf(stderr, "Failed to create UDP socket\n");
        co_return;
    }

    sock.set_reuse_address(true);

    udp::endpoint listen_ep(port);
    if (!sock.bind(listen_ep)) {
        fprintf(stderr, "Failed to bind to port %u\n", port);
        co_return;
    }
    printf("Listening for broadcasts on port %u...\n", port);

    char buf[1500];
    udp::endpoint sender;
    while (true) {
        auto n = co_await sock.async_receive_from(buffer(buf, sizeof(buf)), sender);
        if (n <= 0) break;
        printf("[%s:%u] %.*s\n",
               sender.address().c_str(), sender.port(),
               static_cast<int>(n), buf);
    }
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(atoi(argv[1])) : 6000;

    try {
        io_context ctx;
        auto* task = new Task<void>(run_receiver(ctx, port));
        task->resume();
        if (task->done()) { delete task; return 0; }
        ctx.run();
        delete task;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
