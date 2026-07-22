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
        backend_->poll(next_timer_timeout_ms());
        process_timers();
        execute_pending();
    }

    current_context_ = prev;
}

void io_context::run_until_complete() {
    auto prev = current_context_;
    current_context_ = this;

    while (!stopped_.load(std::memory_order_relaxed)) {
        // Check if all work is done
        if (work_count_.load(std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_.empty()) break;
        }

        backend_->poll(next_timer_timeout_ms());
        process_timers();
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
    backend_->poll(next_timer_timeout_ms());
    process_timers();
    execute_pending();

    current_context_ = prev;
    return handlers_executed_;
}

size_t io_context::poll() {
    auto prev = current_context_;
    current_context_ = this;

    handlers_executed_ = 0;
    backend_->poll(0);  // Non-blocking
    process_timers();
    execute_pending();

    current_context_ = prev;
    return handlers_executed_;
}

void io_context::stop() {
    stopped_.store(true, std::memory_order_relaxed);
    // Wake up the backend if it's blocked in poll()
    backend_->wake();
}

void io_context::post(std::function<void()> func) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.push(std::move(func));
    }
    // Wake up the event loop if it's blocked in poll()
    backend_->wake();
}

void io_context::post_at(std::chrono::steady_clock::time_point deadline,
                          std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(timers_mutex_);
        timers_.push(TimerEntry{deadline, std::move(cb)});
    }
    backend_->wake();
}

void io_context::process_timers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::function<void()>> expired;

    {
        std::lock_guard<std::mutex> lock(timers_mutex_);
        while (!timers_.empty() && timers_.top().deadline <= now) {
            expired.push_back(std::move(const_cast<TimerEntry&>(timers_.top()).callback));
            timers_.pop();
        }
    }

    for (auto& cb : expired) {
        cb();
        ++handlers_executed_;
    }
}

int io_context::next_timer_timeout_ms() const {
    std::lock_guard<std::mutex> lock(timers_mutex_);
    if (timers_.empty()) return 100;  // default 100ms

    auto now = std::chrono::steady_clock::now();
    auto diff = timers_.top().deadline - now;
    if (diff <= std::chrono::milliseconds::zero()) return 0;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff).count();
    return static_cast<int>(ms);
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
    // Check if user wants to force a specific backend via environment variable
    const char* backend_env = std::getenv("ASYNC_NET_BACKEND");
    if (backend_env) {
        std::string backend_name = backend_env;
        if (backend_name == "epoll") {
            return std::make_unique<EpollBackend>();
        } else if (backend_name == "io_uring") {
            try {
                return std::make_unique<IoUringBackend>();
            } catch (const std::exception& e) {
                // Fall through to auto-detection
            }
        }
    }

    // Auto-detect: try io_uring first (requires kernel 5.1+), fall back to epoll
    try {
        auto backend = std::make_unique<IoUringBackend>();
        return backend;
    } catch (const std::exception&) {
        // io_uring not supported by kernel, fall back to epoll
        return std::make_unique<EpollBackend>();
    }
#elif defined(ASYNC_NET_MACOS) || defined(ASYNC_NET_BSD)
    return std::make_unique<KqueueBackend>();
#elif defined(ASYNC_NET_WINDOWS)
    return std::make_unique<IocpBackend>();
#else
    return nullptr;
#endif
}

} // namespace async_net
