// Broadcast sender — sends UDP broadcast messages.
// Usage: ./broadcast_sender [port] [count] [dest]
//   port:  UDP broadcast port (default 6000)
//   count: number of messages  (default 3)
//   dest:  broadcast address   (default 127.255.255.255)

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/udp.hpp>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace async_net;

Task<void> run_sender(io_context& ctx, uint16_t port, int count, const char* dest) {
    udp::socket sock(ctx);
    if (!sock.is_open()) {
        fprintf(stderr, "Failed to create UDP socket\n");
        co_return;
    }

    sock.set_broadcast(true);

    udp::endpoint dest_ep(port, dest);
    printf("Sending %d broadcast messages to %s:%u\n", count, dest, port);

    for (int i = 1; i <= count; ++i) {
        char msg[128];
        int len = snprintf(msg, sizeof(msg), "Broadcast message #%d", i);
        auto n = co_await sock.async_send_to(const_buffer(msg, len), dest_ep);
        if (n > 0) {
            printf("Sent (%zd bytes): %s\n", n, msg);
        } else {
            fprintf(stderr, "Send failed: %zd\n", n);
        }
        ::sleep(1);
    }

    printf("Done.\n");
    co_return;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(atoi(argv[1])) : 6000;
    int count = (argc > 2) ? atoi(argv[2]) : 3;
    const char* dest = (argc > 3) ? argv[3] : "127.255.255.255";

    try {
        io_context ctx;
        auto* task = new Task<void>(run_sender(ctx, port, count, dest));
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
