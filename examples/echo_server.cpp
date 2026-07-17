#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <async_net/net/buffer.hpp>
#include <async_net/coroutine/task.hpp>
#include <iostream>
#include <cstring>
#include <unordered_set>

using namespace async_net;

// Global set to keep fire-and-forget tasks alive
static std::unordered_set<Task<void>*> active_tasks;

Task<void> handle_client(tcp::socket sock) {
    char buf[4096];
    int client_num = sock.native_handle();
    std::cout << "[Server] Client " << client_num << " connected" << std::endl;

    while (true) {
        auto n = co_await sock.async_read_some(mutable_buffer(buf, sizeof(buf)));
        if (n <= 0) {
            std::cout << "[Server] Client " << client_num << " disconnected (n=" << n << ")" << std::endl;
            break;
        }

        std::cout << "[Server] Received " << n << " bytes from client " << client_num << std::endl;

        // Echo back
        auto written = co_await sock.async_write_some(const_buffer(buf, n));
        if (written <= 0) {
            std::cout << "[Server] Write failed for client " << client_num << std::endl;
            break;
        }
        std::cout << "[Server] Echoed " << written << " bytes to client " << client_num << std::endl;
    }
}

Task<void> run_server(io_context& ctx, uint16_t port) {
    tcp::acceptor acc(ctx, port);
    if (!acc.is_open()) {
        std::cerr << "[Server] Failed to open acceptor on port " << port << std::endl;
        co_return;
    }

    std::cout << "[Server] Listening on port " << port << std::endl;

    while (true) {
        auto sock = co_await acc.async_accept();
        if (!sock.is_open()) {
            std::cerr << "[Server] Accept failed" << std::endl;
            co_return;
        }

        // Create a heap-allocated task to keep it alive
        auto* task = new Task<void>(handle_client(std::move(sock)));
        active_tasks.insert(task);

        // Start the task - it will run until first co_await then suspend
        task->resume();

        // If the task completed synchronously, clean it up
        if (task->done()) {
            active_tasks.erase(task);
            delete task;
        }
    }
}

// Cleanup helper - called when a task completes
void cleanup_task(Task<void>* task) {
    active_tasks.erase(task);
    delete task;
}

int main(int argc, char* argv[]) {
    uint16_t port = 6543;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    try {
        io_context ctx;
        std::cout << "[Server] Using backend: " << ctx.backend().name() << std::endl;

        // Start the server task
        auto server_task = run_server(ctx, port);
        server_task.resume();

        // Run the event loop
        ctx.run();
    } catch (const std::exception& e) {
        std::cerr << "[Server] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
