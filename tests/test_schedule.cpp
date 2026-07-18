// Test executor schedule / timer functionality
#include <async_net/io/io_context.hpp>
#include <async_net/executor/schedule.hpp>
#include <async_net/executor/coroutine.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/coroutine/spawn.hpp>
#include <iostream>
#include <cassert>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

using namespace async_net;
using namespace std::chrono;

// ============================================================================
// Test: post_after — basic delayed callback
// ============================================================================
void test_post_after() {
    std::cout << "test_post_after:" << std::endl;

    io_context ctx;
    std::atomic<int> count{0};
    auto start = steady_clock::now();
    steady_clock::time_point fired_at;

    ctx.post_after(100ms, [&] {
        fired_at = steady_clock::now();
        count++;
    });

    // Run until the timer fires and work is done
    auto guard = ctx.make_work();
    ctx.post_after(200ms, [&] {
        guard.reset();  // release work after the first timer should have fired
    });

    ctx.run_until_complete();

    auto elapsed = duration_cast<milliseconds>(fired_at - start);
    assert(count.load() == 1);
    assert(elapsed >= 80ms);  // allow some timing slack
    std::cout << "  Timer fired after " << elapsed.count() << "ms OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: post_at — callback at specific time point
// ============================================================================
void test_post_at() {
    std::cout << "test_post_at:" << std::endl;

    io_context ctx;
    std::atomic<int> count{0};

    auto deadline = steady_clock::now() + 50ms;
    ctx.post_at(deadline, [&] { count++; });

    auto guard = ctx.make_work();
    ctx.post_at(deadline + 50ms, [&] { guard.reset(); });

    ctx.run_until_complete();
    assert(count.load() == 1);
    std::cout << "  post_at fired OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: sleep_for — coroutine sleep
// ============================================================================
Task<void> sleep_task(io_context& ctx, steady_clock::time_point& wake_time,
                       std::atomic<bool>& done) {
    auto start = steady_clock::now();
    co_await sleep_for(100ms, ctx);
    wake_time = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(wake_time - start);
    (void)elapsed;
    done.store(true);
}

void test_sleep_for() {
    std::cout << "test_sleep_for:" << std::endl;

    io_context ctx;
    steady_clock::time_point wake_time;
    std::atomic<bool> done{false};

    auto start = steady_clock::now();
    auto task = sleep_task(ctx, wake_time, done);
    task.resume();  // starts, hits co_await sleep_for, suspends

    auto guard = ctx.make_work();
    ctx.post_after(200ms, [&] { guard.reset(); });

    ctx.run_until_complete();

    auto elapsed = duration_cast<milliseconds>(wake_time - start);
    assert(done.load());
    assert(elapsed >= 80ms);
    std::cout << "  Coroutine slept for ~" << elapsed.count() << "ms OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: sleep_for with io_context::current()
// ============================================================================
Task<void> sleep_task_implicit(std::atomic<bool>& done,
                                steady_clock::time_point& wake_time) {
    auto start = steady_clock::now();
    co_await sleep_for(50ms);  // uses io_context::current()
    wake_time = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(wake_time - start);
    (void)elapsed;
    done.store(true);
}

void test_sleep_for_implicit() {
    std::cout << "test_sleep_for_implicit:" << std::endl;

    io_context ctx;
    std::atomic<bool> done{false};
    steady_clock::time_point wake_time;

    // Store the task to keep it alive across the suspend
    auto stored_task = std::make_shared<Task<void>>();

    ctx.post([&] {
        *stored_task = sleep_task_implicit(done, wake_time);
        stored_task->resume();
    });

    auto guard = ctx.make_work();
    ctx.post_after(150ms, [&] { guard.reset(); });
    ctx.run_until_complete();

    assert(done.load());
    std::cout << "  sleep_for with implicit io_context OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: schedule — one-shot delayed task
// ============================================================================
Task<int> delayed_compute(int value) {
    co_return value * 2;
}

void test_schedule_oneshot() {
    std::cout << "test_schedule_oneshot:" << std::endl;

    io_context ctx;
    auto start = steady_clock::now();

    auto handle = schedule(ctx, 100ms, delayed_compute(21));

    // Wait for result via a wrapper task
    std::atomic<bool> done{false};
    int result = 0;

    auto checker = [&done, &result](JoinHandle<int>* h) -> Task<void> {
        result = co_await std::move(*h);
        done.store(true);
    };
    auto t = checker(&handle);
    t.resume();

    auto guard = ctx.make_work();
    ctx.post_after(300ms, [&] { guard.reset(); });
    ctx.run_until_complete();

    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start);
    assert(done.load());
    assert(result == 42);
    assert(elapsed >= 80ms);
    std::cout << "  schedule returned " << result << " after ~" << elapsed.count() << "ms OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: schedule_at_fixed_rate — periodic task with cancel
// ============================================================================
Task<void> periodic_tick(std::atomic<int>& counter) {
    counter.fetch_add(1);
    co_return;
}

void test_schedule_periodic() {
    std::cout << "test_schedule_at_fixed_rate:" << std::endl;

    io_context ctx;
    std::atomic<int> counter{0};

    // Schedule periodic task: first run after 50ms, then every 50ms
    auto task = schedule_at_fixed_rate(ctx, 50ms,
        [&] { return periodic_tick(counter); }, 50ms);

    // Let it run for ~300ms, then cancel
    ctx.post_after(320ms, [&] {
        task.cancel();
        ctx.stop();
    });
    ctx.run();

    int count = counter.load();
    // Should have fired ~5-6 times (50ms initial + 4-5 intervals of 50ms in 320ms)
    std::cout << "  Periodic counter = " << count << std::endl;
    assert(count >= 4);  // at least 4 (allow timing slack)
    assert(count <= 8);  // not too many
    assert(task.is_cancelled());
    std::cout << "  Task cancelled OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: schedule_once — one-shot with cancel
// ============================================================================
Task<void> delayed_increment(std::atomic<int>& counter) {
    counter.fetch_add(1);
    co_return;
}

void test_schedule_once_cancel() {
    std::cout << "test_schedule_once_cancel:" << std::endl;

    io_context ctx;
    std::atomic<int> counter{0};

    // Schedule a task after 200ms
    auto task = schedule_once(ctx, 200ms, delayed_increment(counter));

    // Cancel it after 50ms (before it fires)
    ctx.post_after(50ms, [&] {
        task.cancel();
    });

    // Check after 300ms that it didn't fire
    auto guard = ctx.make_work();
    ctx.post_after(300ms, [&] { guard.reset(); });
    ctx.run_until_complete();

    assert(counter.load() == 0);  // should NOT have fired
    assert(task.is_cancelled());
    std::cout << "  Cancelled task did not fire OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: multiple concurrent timers
// ============================================================================
void test_multiple_timers() {
    std::cout << "test_multiple_timers:" << std::endl;

    io_context ctx;
    std::vector<int> order;
    std::mutex mtx;

    auto record = [&](int id) {
        std::lock_guard<std::mutex> lk(mtx);
        order.push_back(id);
    };

    // Post timers in reverse order — they should fire in order
    ctx.post_after(300ms, [&] { record(3); });
    ctx.post_after(100ms, [&] { record(1); });
    ctx.post_after(200ms, [&] { record(2); });

    auto guard = ctx.make_work();
    ctx.post_after(400ms, [&] { guard.reset(); });
    ctx.run_until_complete();

    assert(order.size() == 3);
    assert(order[0] == 1);
    assert(order[1] == 2);
    assert(order[2] == 3);
    std::cout << "  Timers fired in correct order: 1,2,3 OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: zero-duration sleep returns immediately
// ============================================================================
Task<void> zero_sleep(std::atomic<bool>& done) {
    auto start = steady_clock::now();
    co_await sleep_for(0ms);
    auto elapsed = duration_cast<microseconds>(steady_clock::now() - start);
    (void)elapsed;
    done.store(true);
}

void test_zero_sleep() {
    std::cout << "test_zero_sleep:" << std::endl;

    io_context ctx;
    std::atomic<bool> done{false};

    // Store the task to keep it alive
    auto stored_task = std::make_shared<Task<void>>();

    ctx.post([&] {
        *stored_task = zero_sleep(done);
        stored_task->resume();
    });

    auto guard = ctx.make_work();
    ctx.post_after(50ms, [&] { guard.reset(); });
    ctx.run_until_complete();

    assert(done.load());
    std::cout << "  Zero-duration sleep returned immediately OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: cross-thread post_after wakes event loop
// ============================================================================
void test_cross_thread_timer() {
    std::cout << "test_cross_thread_timer:" << std::endl;

    io_context ctx;
    std::atomic<bool> fired{false};

    std::thread runner([&] { ctx.run(); });

    // Post timer from another thread
    std::this_thread::sleep_for(10ms);
    ctx.post_after(50ms, [&] {
        fired.store(true);
        ctx.stop();
    });

    runner.join();
    assert(fired.load());
    std::cout << "  Cross-thread timer fired OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    try {
        test_post_after();
        test_post_at();
        test_sleep_for();
        test_sleep_for_implicit();
        test_schedule_oneshot();
        test_schedule_periodic();
        test_schedule_once_cancel();
        test_multiple_timers();
        test_zero_sleep();
        test_cross_thread_timer();
        std::cout << "\nAll schedule tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
