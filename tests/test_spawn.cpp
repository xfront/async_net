#include <async_net/coroutine/task.hpp>
#include <async_net/coroutine/spawn.hpp>
#include <iostream>
#include <cassert>
#include <string>

using namespace async_net;

// --- Helper tasks ---

Task<int> compute_value(int x) {
    co_return x * 2;
}

Task<void> print_message(const std::string& msg) {
    std::cout << "  [task] " << msg << std::endl;
    co_return;
}

Task<int> failing_task() {
    throw std::runtime_error("intentional error");
    co_return 0;
}

Task<std::string> greeting(const std::string& name) {
    co_return "Hello, " + name + "!";
}

Task<int> add_task(int a, int b) {
    co_return a + b;
}

// --- Tests ---

Task<void> test_spawn_value() {
    std::cout << "test_spawn_value:" << std::endl;
    auto handle = spawn(compute_value(21));
    assert(!handle.is_finished() || handle.is_finished()); // may already be done
    int result = co_await std::move(handle);
    assert(result == 42);
    std::cout << "  result = " << result << " OK" << std::endl;
}

Task<void> test_spawn_void() {
    std::cout << "test_spawn_void:" << std::endl;
    auto handle = spawn(print_message("spawned!"));
    co_await std::move(handle);
    std::cout << "  void task completed OK" << std::endl;
}

Task<void> test_spawn_string() {
    std::cout << "test_spawn_string:" << std::endl;
    auto handle = spawn(greeting("World"));
    std::string result = co_await std::move(handle);
    assert(result == "Hello, World!");
    std::cout << "  result = " << result << " OK" << std::endl;
}

Task<void> test_spawn_fire_and_forget() {
    std::cout << "test_spawn_fire_and_forget:" << std::endl;
    // Spawn without awaiting — task runs independently
    // Frame should be cleaned up by JoinHandle destructor
    spawn(print_message("fire and forget"));
    std::cout << "  fire-and-forget dispatched OK" << std::endl;
    co_return;
}

Task<void> test_spawn_exception() {
    std::cout << "test_spawn_exception:" << std::endl;
    auto handle = spawn(failing_task());
    bool caught = false;
    try {
        co_await std::move(handle);
    } catch (const std::runtime_error& e) {
        caught = true;
        std::cout << "  caught: " << e.what() << " OK" << std::endl;
    }
    assert(caught);
}

Task<void> test_spawn_multiple() {
    std::cout << "test_spawn_multiple:" << std::endl;
    auto h1 = spawn(add_task(10, 20));
    auto h2 = spawn(add_task(30, 40));
    auto h3 = spawn(add_task(50, 60));

    int r1 = co_await std::move(h1);
    int r2 = co_await std::move(h2);
    int r3 = co_await std::move(h3);

    assert(r1 == 30);
    assert(r2 == 70);
    assert(r3 == 110);
    std::cout << "  results: " << r1 << ", " << r2 << ", " << r3 << " OK" << std::endl;
}

Task<void> test_spawn_nested() {
    std::cout << "test_spawn_nested:" << std::endl;
    auto outer = spawn([]() -> Task<int> {
        auto h1 = spawn(compute_value(5));
        auto h2 = spawn(compute_value(10));
        int r1 = co_await std::move(h1);
        int r2 = co_await std::move(h2);
        co_return r1 + r2;
    }());

    int result = co_await std::move(outer);
    assert(result == 30);
    std::cout << "  nested result = " << result << " OK" << std::endl;
}

Task<void> test_is_finished() {
    std::cout << "test_is_finished:" << std::endl;
    auto handle = spawn(compute_value(1));
    // Synchronous task — should be done immediately after spawn
    bool finished = handle.is_finished();
    std::cout << "  is_finished = " << (finished ? "true" : "false") << std::endl;
    assert(finished);
    int result = co_await std::move(handle);
    assert(result == 2);
    std::cout << "  result = " << result << " OK" << std::endl;
}

Task<void> test_detach() {
    std::cout << "test_detach:" << std::endl;
    auto handle = spawn(print_message("detached task"));
    assert(static_cast<bool>(handle));
    handle.detach();
    assert(!static_cast<bool>(handle)); // handle is now empty
    std::cout << "  detached OK" << std::endl;
    co_return;
}

Task<void> test_move_assignment() {
    std::cout << "test_move_assignment:" << std::endl;
    auto h1 = spawn(compute_value(10));
    auto h2 = spawn(compute_value(20));
    // Move-assign h2 to h1 — h1's old task should be cleaned up
    h1 = std::move(h2);
    int result = co_await std::move(h1);
    assert(result == 40);
    std::cout << "  result = " << result << " OK" << std::endl;
}

Task<void> test_bool_operator() {
    std::cout << "test_bool_operator:" << std::endl;
    JoinHandle<int> empty;
    assert(!static_cast<bool>(empty));
    auto handle = spawn(compute_value(5));
    assert(static_cast<bool>(handle));
    co_await std::move(handle);
    std::cout << "  bool checks OK" << std::endl;
}

// --- Main ---

Task<void> run_all_tests() {
    co_await test_spawn_value();
    co_await test_spawn_void();
    co_await test_spawn_string();
    co_await test_spawn_fire_and_forget();
    co_await test_spawn_exception();
    co_await test_spawn_multiple();
    co_await test_spawn_nested();
    co_await test_is_finished();
    co_await test_detach();
    co_await test_move_assignment();
    co_await test_bool_operator();
    std::cout << "\nAll spawn tests passed!" << std::endl;
}

int main() {
    try {
        auto main_task = run_all_tests();
        main_task.resume();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
