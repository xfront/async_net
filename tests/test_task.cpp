#include <async_net/coroutine/task.hpp>
#include <iostream>
#include <cassert>

using namespace async_net;

Task<int> simple_task() {
    co_return 42;
}

Task<void> void_task() {
    co_return;
}

Task<int> nested_task() {
    int val = co_await simple_task();
    co_return val * 2;
}

Task<void> chain_task() {
    int val = co_await nested_task();
    assert(val == 84);
    std::cout << "Nested task returned: " << val << std::endl;
    co_return;
}

Task<int> exception_task() {
    throw std::runtime_error("test error");
    co_return 0;  // unreachable
}

Task<void> test_basic() {
    std::cout << "test_basic: start" << std::endl;
    // Test simple task
    {
        auto t1 = simple_task();
        std::cout << "test_basic: resuming t1" << std::endl;
        t1.resume();
        std::cout << "test_basic: t1 done" << std::endl;
        assert(t1.done());
    }
    std::cout << "test_basic: t1 destroyed" << std::endl;

    // Test nested task
    {
        auto t2 = chain_task();
        std::cout << "test_basic: resuming t2" << std::endl;
        t2.resume();
        std::cout << "test_basic: t2 done" << std::endl;
        assert(t2.done());
    }
    std::cout << "test_basic: t2 destroyed" << std::endl;

    std::cout << "All basic tests passed!" << std::endl;
    co_return;
}

int main() {
    try {
        auto main_task = test_basic();
        main_task.resume();

        std::cout << "All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
