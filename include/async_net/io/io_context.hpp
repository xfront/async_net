#pragma once

#include "io_backend.hpp"
#include "../coroutine/task.hpp"
#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <atomic>

namespace async_net {

class io_context {
public:
    io_context();
    explicit io_context(std::unique_ptr<IoBackend> backend);
    ~io_context();

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    // Run the event loop. Blocks until stop() is called.
    void run();

    // Run one iteration of the event loop. Returns number of handlers executed.
    size_t run_one();

    // Poll for ready events without blocking. Returns number of handlers executed.
    size_t poll();

    // Stop the event loop
    void stop();

    // Check if stopped
    bool stopped() const { return stopped_.load(std::memory_order_relaxed); }

    // Reset after stop
    void restart() { stopped_.store(false, std::memory_order_relaxed); }

    // Post a function to be executed in the event loop
    void post(std::function<void()> func);

    // Get the backend
    IoBackend& backend() { return *backend_; }
    const IoBackend& backend() const { return *backend_; }

    // Get the current io_context for this thread (if any)
    static io_context* current() { return current_context_; }

private:
    void execute_pending();

    std::unique_ptr<IoBackend> backend_;
    std::atomic<bool> stopped_{false};
    size_t handlers_executed_ = 0;

    // Pending function queue
    std::queue<std::function<void()>> pending_;
    std::mutex pending_mutex_;

    // Thread-local current context
    static thread_local io_context* current_context_;
};

} // namespace async_net
