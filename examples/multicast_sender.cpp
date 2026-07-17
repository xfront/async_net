// Multicast sender — sends periodic messages to a multicast group.
// Usage: ./multicast_sender [group] [port] [count] [interface]
//   group:     multicast group address (default 239.0.0.1)
//   port:      UDP port to send to     (default 5000)
//   count:     number of messages      (default 5)
//   interface: outgoing interface      (default 127.0.0.1)

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/udp.hpp>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace async_net;

Task<void> run_sender(io_context& ctx, const char* group, uint16_t port, int count, const char* iface) {
    udp::socket sock(ctx);
    if (!sock.is_open()) {
        fprintf(stderr, "Failed to create UDP socket\n");
        co_return;
    }

    // Set multicast TTL (1 = local subnet) and outgoing interface
    sock.set_multicast_ttl(1);
    sock.set_multicast_loopback(true); // receive our own messages
    sock.set_multicast_interface(iface);

    udp::endpoint dest(port, group);
    printf("Sending %d messages to %s:%u\n", count, group, port);

    for (int i = 1; i <= count; ++i) {
        char msg[128];
        int len = snprintf(msg, sizeof(msg), "Multicast message #%d", i);
        auto n = co_await sock.async_send_to(const_buffer(msg, len), dest);
        if (n > 0) {
            printf("Sent (%zd bytes): %s\n", n, msg);
        } else {
            fprintf(stderr, "Send failed: %zd\n", n);
        }
        ::sleep(1);
    }

    printf("Done sending.\n");
    co_return;
}

int main(int argc, char* argv[]) {
    const char* group = (argc > 1) ? argv[1] : "239.0.0.1";
    uint16_t port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 5000;
    int count = (argc > 3) ? atoi(argv[3]) : 5;
    const char* iface = (argc > 4) ? argv[4] : "127.0.0.1";

    try {
        io_context ctx;
        auto* task = new Task<void>(run_sender(ctx, group, port, count, iface));
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
