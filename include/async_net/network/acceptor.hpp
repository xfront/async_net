#pragma once

// ---------------------------------------------------------------------------
// acceptor — Acceptor pattern (passive connection establishment)
//
// Listens on a TCP port, accepts incoming connections, and creates a
// Service Handler for each. The handler lifecycle:
//   make_handler() -> open(peer) -> run() -> handle_close()
//
// Template parameters:
//   SVC_HANDLER   — subclass of service_handler that processes connections
//   PEER_ACCEPTOR — the acceptor factory/strategy (default: tcp::acceptor)
//
// PEER_ACCEPTOR must satisfy:
//   - Constructable with (io_context&, port, addr, reuse_port)
//   - async_accept() returning a peer stream compatible with SVC_HANDLER::peer_stream_type
//   - is_open() -> bool
//   - close()
//
// Usage:
//   class echo_handler : public network::service_handler<> { ... };
//   network::acceptor<echo_handler> acc(ctx, 8080);
//   co_await acc.serve();  // blocks until stop()
//
// For custom handler construction (e.g., passing extra context):
//   struct my_acceptor : network::acceptor<my_handler> {
//       pointer make_handler() override {
//           return std::make_shared<my_handler>(ctx_);
//       }
//   };
// ---------------------------------------------------------------------------

#include <async_net/network/service_handler.hpp>
#include <async_net/io/tcp.hpp>
#include <async_net/coroutine/task.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>

namespace async_net::network {

template<typename SVC_HANDLER, typename PEER_ACCEPTOR = tcp::acceptor>
class acceptor {
    static_assert(std::is_base_of_v<service_handler<typename SVC_HANDLER::peer_stream_type>, SVC_HANDLER>,
                  "SVC_HANDLER must inherit from service_handler");
    static_assert(std::is_same_v<typename SVC_HANDLER::peer_stream_type, typename PEER_ACCEPTOR::peer_stream_type>,
                  "SVC_HANDLER::peer_stream_type must match PEER_ACCEPTOR::peer_stream_type");

public:
    using handler_type = SVC_HANDLER;
    using peer_acceptor_type = PEER_ACCEPTOR;
    using peer_stream_type = typename handler_type::peer_stream_type;
    using handler_ptr = std::shared_ptr<handler_type>;

    acceptor(io_context& ctx, uint16_t port,
             const char* addr = "0.0.0.0", bool reuse_port = false)
        : ctx_(&ctx), port_(port), addr_(addr ? addr : "0.0.0.0"), reuse_port_(reuse_port) {}

    virtual ~acceptor() = default;

    /// Set a factory function for creating handlers.
    /// Alternative to overriding make_handler() in a derived class.
    void set_factory(std::function<handler_ptr()> f) { factory_ = std::move(f); }

    /// Factory method — override to customize handler creation (e.g., pass shared state).
    /// Uses if constexpr to avoid instantiation errors for non-default-constructible handlers.
    virtual handler_ptr make_handler() {
        if constexpr (std::is_default_constructible_v<handler_type>) {
            return std::make_shared<handler_type>();
        }
        if (factory_) return factory_();
        return nullptr;
    }

    /// Accept loop — runs until stop() or unrecoverable error.
    /// Returns when all active handlers have completed.
    Task<void> serve() {
        PEER_ACCEPTOR acc(*ctx_, port_, addr_.c_str(), reuse_port_);
        if (!acc.is_open()) {
            std::cerr << "[acceptor] Failed to bind " << addr_ << ":" << port_ << std::endl;
            co_return;
        }
        std::cout << "[acceptor] Listening on " << addr_ << ":" << port_ << std::endl;

        auto work = ctx_->make_work();

        while (running_) {
            auto peer = co_await acc.async_accept();
            if (!peer.is_open()) {
                if (!running_) break;
                continue;
            }

            auto handler = make_handler();
            handler->open(std::move(peer));
            launch_handler(std::move(handler));
        }

        // Wait for all handlers to complete
        await_completion();
    }

    void stop() {
        running_ = false;
        // Wake the io_context so poll() unblocks and serve() can check running_
        ctx_->wake();
    }

    io_context& get_io_context() { return *ctx_; }

private:
    struct task_entry {
        handler_ptr handler;
        Task<void> task;
    };

    void launch_handler(handler_ptr handler) {
        auto entry = std::make_unique<task_entry>();
        entry->handler = std::move(handler);
        entry->task = entry->handler->run();

        // Resume the coroutine
        entry->task.resume();

        if (entry->task.done()) {
            // Completed synchronously — handler already cleaned up
            entry->handler = nullptr;
            // task destroyed here
        } else {
            // Suspendend — keep alive until completion
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.insert(entry.release());
        }

        // Opportunistically clean up completed tasks
        reap_completed();
    }

    void reap_completed() {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if ((*it)->task.done()) {
                delete *it;
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void await_completion() {
        // Spin with short polls until all tasks complete
        while (true) {
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                if (tasks_.empty()) break;
            }
            // Give time for completion callbacks to fire
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            reap_completed();
        }
    }

    io_context* ctx_;
    uint16_t port_ = 0;
    std::string addr_;
    bool reuse_port_ = false;
    bool running_ = true;
    std::function<handler_ptr()> factory_;

    std::unordered_set<task_entry*> tasks_;
    std::mutex tasks_mutex_;
};

} // namespace async_net::network
