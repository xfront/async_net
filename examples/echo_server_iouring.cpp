// Test program for io_uring backend
// Usage: Run this, then in another terminal run ./echo_client

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <cstdio>
#include <cstring>
#include <unordered_set>

// Access the io_uring backend directly
#include "io/io_uring_backend.hpp"

using namespace async_net;

static std::unordered_set<Task<void>*> active_tasks;

Task<void> handle_client(tcp::socket sock) {
    char buf[1024];
    while (true) {
        auto n = co_await sock.async_read_some(buffer(buf, sizeof(buf)));
        if (n <= 0) break;
        co_await sock.async_write(const_buffer(buf, static_cast<size_t>(n)));
    }
    co_return;
}

Task<void> run_server(io_context& ctx, uint16_t port) {
    tcp::acceptor acc(ctx, port);
    if (!acc.is_open()) {
        fprintf(stderr, "Failed to open acceptor on port %u\n", port);
        co_return;
    }
    printf("[io_uring Server] Listening on port %u\n", port);

    while (true) {
        auto sock = co_await acc.async_accept();
        if (!sock.is_open()) continue;

        auto* task = new Task<void>(handle_client(std::move(sock)));
        active_tasks.insert(task);
        task->resume();
        if (task->done()) {
            active_tasks.erase(task);
            delete task;
        }
    }
    co_return;
}

int main() {
    try {
        // Create io_context with io_uring backend
        auto backend = std::make_unique<IoUringBackend>(256);
        printf("[Server] Using backend: %s\n", backend->name());

        io_context ctx(std::move(backend));
        auto* server_task = new Task<void>(run_server(ctx, 6543));
        server_task->resume();
        if (server_task->done()) {
            delete server_task;
            return 0;
        }
        ctx.run();
        delete server_task;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
