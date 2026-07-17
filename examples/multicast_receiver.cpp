// Multicast receiver — joins a multicast group and prints received messages.
// Usage: ./multicast_receiver [group] [port] [interface]
//   group:     multicast group address (default 239.0.0.1)
//   port:      UDP port to listen on     (default 5000)
//   interface: local interface to bind    (default 127.0.0.1)

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/udp.hpp>
#include <cstdio>
#include <cstring>

using namespace async_net;

Task<void> run_receiver(io_context& ctx, const char* group, uint16_t port, const char* iface) {
    udp::socket sock(ctx);
    if (!sock.is_open()) {
        fprintf(stderr, "Failed to create UDP socket\n");
        co_return;
    }

    sock.set_reuse_address(true);
    sock.set_reuse_port(true);
    sock.set_multicast_interface(iface);

    udp::endpoint listen_ep(port);
    if (!sock.bind(listen_ep)) {
        fprintf(stderr, "Failed to bind to port %u\n", port);
        co_return;
    }

    if (!sock.join_multicast_group(group, iface)) {
        fprintf(stderr, "Failed to join multicast group %s on %s\n", group, iface);
        co_return;
    }
    printf("Joined multicast group %s on interface %s, port %u\n", group, iface, port);

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
    const char* group = (argc > 1) ? argv[1] : "239.0.0.1";
    uint16_t port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 5000;
    const char* iface = (argc > 3) ? argv[3] : "127.0.0.1";

    try {
        io_context ctx;
        auto* task = new Task<void>(run_receiver(ctx, group, port, iface));
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
