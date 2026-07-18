// Test executor / multi-threading support
#include <async_net/io/io_context.hpp>
#include <async_net/executor/executor.hpp>
#include <async_net/executor/thread_pool_executor.hpp>
#include <async_net/executor/strand.hpp>
#include <async_net/executor/coroutine.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/coroutine/spawn.hpp>
#include <iostream>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>
#include <mutex>

using namespace async_net;

// ============================================================================
// Test: thread_pool_executor
// ============================================================================
void test_thread_pool_executor() {
    std::cout << "test_thread_pool_executor:" << std::endl;

    thread_pool pool(4);
    thread_pool_executor pool_exec(pool);

    std::atomic<int> count{0};
    std::mutex mtx;
    std::set<std::thread::id> thread_ids;

    for (int i = 0; i < 20; ++i) {
        pool_exec.post([&] {
            std::lock_guard lk(mtx);
            thread_ids.insert(std::this_thread::get_id());
            count++;
        });
    }

    // Wait for all tasks to complete
    while (count.load() < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(count.load() == 20);
    std::cout << "  All 20 tasks executed OK" << std::endl;
    std::cout << "  Used " << thread_ids.size() << " distinct threads OK" << std::endl;
    assert(thread_ids.size() > 1);
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: io_context as executor
// ============================================================================
void test_io_context_executor() {
    std::cout << "test_io_context_executor:" << std::endl;

    io_context ctx;
    std::atomic<int> count{0};

    for (int i = 0; i < 5; ++i) {
        ctx.post([&] { count++; });
    }

    std::thread runner([&] { ctx.run(); });

    while (count.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(count.load() == 5);
    std::cout << "  All 5 posted tasks executed OK" << std::endl;

    ctx.stop();
    runner.join();
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: run_on — switch execution context
// ============================================================================
Task<void> run_on_test_task(executor& pool_exec, io_context& ctx,
                             std::thread::id& pool_tid, std::thread::id main_tid,
                             std::atomic<bool>& done) {
    auto start_tid = std::this_thread::get_id();
    assert(start_tid == main_tid);

    // Switch to pool executor
    co_await run_on(pool_exec);
    pool_tid = std::this_thread::get_id();
    assert(pool_tid != main_tid);

    // Switch back to io_context (captured before switch)
    co_await run_on(ctx);
    auto back_tid = std::this_thread::get_id();
    (void)back_tid;

    done.store(true);
}

void test_run_on() {
    std::cout << "test_run_on:" << std::endl;

    io_context ctx;
    thread_pool pool(2);
    thread_pool_executor pool_exec(pool);
    auto main_tid = std::this_thread::get_id();

    std::thread::id pool_tid{};
    std::atomic<bool> done{false};

    // Start the task on main thread
    auto task = run_on_test_task(pool_exec, ctx, pool_tid, main_tid, done);
    task.resume();

    // Run io_context to process the run_on callbacks
    std::thread runner([&] { ctx.run(); });

    // Wait for task to complete
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(pool_tid != main_tid);
    std::cout << "  Switched to pool thread OK" << std::endl;
    std::cout << "  Task completed after executor switches OK" << std::endl;

    ctx.stop();
    runner.join();
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: co_spawn — start coroutine on executor
// ============================================================================
Task<int> compute_on_pool() {
    co_return 42;
}

void test_co_spawn() {
    std::cout << "test_co_spawn:" << std::endl;

    thread_pool pool(2);
    thread_pool_executor pool_exec(pool);

    // co_spawn on pool executor — the task runs on the pool thread
    auto jh = co_spawn(pool_exec, compute_on_pool());

    // Wait for result
    while (!jh.is_finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify result by co_awaiting in a wrapper
    auto check = [](JoinHandle<int>* jhp) -> Task<void> {
        int val = co_await std::move(*jhp);
        assert(val == 42);
    };
    auto t = check(&jh);
    t.resume();
    assert(t.done());

    std::cout << "  co_spawn returned correct value (42) OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: strand — serialized execution
// ============================================================================
void test_strand() {
    std::cout << "test_strand:" << std::endl;

    thread_pool pool(4);
    thread_pool_executor pool_exec(pool);
    strand s(pool_exec);

    std::atomic<int> counter{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> current_concurrent{0};

    for (int i = 0; i < 50; ++i) {
        s.post([&] {
            int c = current_concurrent.fetch_add(1) + 1;
            int expected = max_concurrent.load();
            while (c > expected && !max_concurrent.compare_exchange_weak(expected, c)) {}

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            current_concurrent.fetch_sub(1);
            counter++;
        });
    }

    while (counter.load() < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(counter.load() == 50);
    assert(max_concurrent.load() == 1);
    std::cout << "  All 50 tasks executed serially OK" << std::endl;
    std::cout << "  Max concurrent = " << max_concurrent.load() << " OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: multi-thread post wakes io_context
// ============================================================================
void test_multi_thread_post() {
    std::cout << "test_multi_thread_post:" << std::endl;

    io_context ctx;
    std::atomic<int> count{0};

    std::thread runner([&] { ctx.run(); });

    std::vector<std::thread> posters;
    for (int t = 0; t < 4; ++t) {
        posters.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                ctx.post([&] { count++; });
            }
        });
    }

    for (auto& t : posters) t.join();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (count.load() < 40 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(count.load() == 40);
    std::cout << "  All 40 tasks from 4 threads executed OK" << std::endl;

    ctx.stop();
    runner.join();
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: work_guard
// ============================================================================
void test_work_guard() {
    std::cout << "test_work_guard:" << std::endl;

    io_context ctx;

    // Test work count tracking
    {
        auto guard = ctx.make_work();
        assert(ctx.work_count() == 1);

        auto guard2 = guard;
        assert(ctx.work_count() == 2);

        guard2.reset();
        assert(ctx.work_count() == 1);
    }
    assert(ctx.work_count() == 0);
    std::cout << "  work_guard count tracking OK" << std::endl;

    // Test run_until_complete with work_guard
    std::atomic<int> count{0};
    {
        auto guard = ctx.make_work();

        ctx.post([&] {
            count++;
            ctx.post([&] { count++; });
        });

        std::thread runner([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            guard.reset();
        });

        ctx.run_until_complete();
        runner.join();
    }

    assert(count.load() == 2);
    std::cout << "  run_until_complete exited correctly OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    try {
        test_thread_pool_executor();
        test_io_context_executor();
        test_run_on();
        test_co_spawn();
        test_strand();
        test_multi_thread_post();
        test_work_guard();
        std::cout << "\nAll executor tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
