#include <async_net/coroutine/task.hpp>
#include <iostream>

using namespace async_net;

Task<void> void_task() {
    std::cout << "void_task: before return" << std::endl;
    co_return;
}

Task<void> test_basic() {
    std::cout << "test_basic: before co_await" << std::endl;
    co_await void_task();
    std::cout << "test_basic: after co_await" << std::endl;
}

int main() {
    std::cout << "main: creating test_basic" << std::endl;
    auto t = test_basic();
    std::cout << "main: resuming test_basic" << std::endl;
    t.resume();
    std::cout << "main: test_basic done: " << t.done() << std::endl;
    std::cout << "main: before return" << std::endl;
    return 0;
}
