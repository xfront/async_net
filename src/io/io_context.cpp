#include <async_net/io/io_context.hpp>
#include <stdexcept>

#ifdef ASYNC_NET_WINDOWS
#include <winsock2.h>
namespace {
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};
static WinsockInit g_winsock_init;
} // anonymous namespace
#endif

#ifdef ASYNC_NET_LINUX
#include "epoll_backend.hpp"
#include "io_uring_backend.hpp"
#elif defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)
#include "kqueue_backend.hpp"
#elif defined(ASYNC_NET_WINDOWS)
#include "iocp_backend.hpp"
#endif

namespace async_net {

thread_local io_context* io_context::current_context_ = nullptr;

io_context::io_context()
    : backend_(create_default_backend()) {
    if (!backend_) {
        throw std::runtime_error("Failed to create I/O backend");
    }
}

io_context::io_context(std::unique_ptr<IoBackend> backend)
    : backend_(std::move(backend)) {
    if (!backend_) {
        throw std::runtime_error("I/O backend is null");
    }
}

io_context::~io_context() {
    stop();
}

void io_context::run() {
    auto prev = current_context_;
    current_context_ = this;

    while (!stopped_.load(std::memory_order_relaxed)) {
        backend_->poll(100);  // 100ms timeout
        execute_pending();
    }

    current_context_ = prev;
}

size_t io_context::run_one() {
    if (stopped_.load(std::memory_order_relaxed)) {
        return 0;
    }

    auto prev = current_context_;
    current_context_ = this;

    handlers_executed_ = 0;
    backend_->poll(100);
    execute_pending();

    current_context_ = prev;
    return handlers_executed_;
}

size_t io_context::poll() {
    auto prev = current_context_;
    current_context_ = this;

    handlers_executed_ = 0;
    backend_->poll(0);  // Non-blocking
    execute_pending();

    current_context_ = prev;
    return handlers_executed_;
}

void io_context::stop() {
    stopped_.store(true, std::memory_order_relaxed);
}

void io_context::post(std::function<void()> func) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.push(std::move(func));
}

void io_context::execute_pending() {
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        std::swap(pending, pending_);
    }

    while (!pending.empty()) {
        pending.front()();
        pending.pop();
        ++handlers_executed_;
    }
}

// Factory function for default backend
std::unique_ptr<IoBackend> create_default_backend() {
#ifdef ASYNC_NET_LINUX
    return std::make_unique<EpollBackend>();
#elif defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)
    return std::make_unique<KqueueBackend>();
#elif defined(ASYNC_NET_WINDOWS)
    return std::make_unique<IocpBackend>();
#else
    return nullptr;
#endif
}

} // namespace async_net
