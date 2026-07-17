#include <async_net/coroutine/task.hpp>
#include <iostream>
#include <cassert>

using namespace async_net;

Task<int> simple_task() {
    std::cout << "simple_task: before return" << std::endl;
    co_return 42;
}

int main() {
    std::cout << "main: creating task" << std::endl;
    auto t = simple_task();
    std::cout << "main: resuming task" << std::endl;
    t.resume();
    std::cout << "main: task done: " << t.done() << std::endl;
    std::cout << "main: before return" << std::endl;
    return 0;
}
