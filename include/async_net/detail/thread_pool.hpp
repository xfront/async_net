#pragma once

#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <type_traits>

namespace async_net {

class thread_pool {
public:
    explicit thread_pool(size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        if (num_threads == 0) num_threads = 1;
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~thread_pool() {
        stop();
    }

    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    // Submit a callable and get a future. Supports move-only callables.
    template<typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using return_type = decltype(f());

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::forward<F>(f)
        );

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("Cannot submit to stopped thread pool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        condition_.notify_one();
        return result;
    }

    // Get the number of threads
    size_t size() const { return workers_.size(); }

    // Post a simple function (no return value). Thread-safe.
    void post(std::function<void()> func) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;
            tasks_.push(std::move(func));
        }
        condition_.notify_one();
    }

    // Signal all workers to stop and wait for completion.
    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_;
};

} // namespace async_net
