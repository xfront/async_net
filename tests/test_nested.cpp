#include <async_net/coroutine/task.hpp>
#include <iostream>

using namespace async_net;

Task<int> inner_task() {
    std::cout << "inner_task: before return" << std::endl;
    co_return 42;
}

Task<int> outer_task() {
    std::cout << "outer_task: before co_await" << std::endl;
    int val = co_await inner_task();
    std::cout << "outer_task: after co_await, val=" << val << std::endl;
    co_return val * 2;
}

int main() {
    std::cout << "main: creating outer task" << std::endl;
    auto t = outer_task();
    std::cout << "main: resuming outer task" << std::endl;
    t.resume();
    std::cout << "main: outer task done: " << t.done() << std::endl;
    std::cout << "main: before return" << std::endl;
    return 0;
}
