# async_net

一个高性能、头文件友好的 C++20 协程异步网络库，支持跨平台 I/O 后端。

## 特性

- **C++20 协程** — 使用 `co_await` / `co_return` 编写如同步代码风格的异步代码
- **多种 I/O 后端** — 自动选择平台最佳后端：
  - **epoll** — Linux（默认）
  - **io_uring** — Linux 5.1+（纯异步，可选）
  - **kqueue** — macOS / FreeBSD / OpenBSD
  - **IOCP** — Windows
- **HTTP 协议栈** — 完整的 HTTP/1.1、HTTP/2 和 HTTP/3 支持：
  - HTTP/1.1，支持 keep-alive 和连接池
  - HTTP/2，支持多路复用、HPACK 和 Server Push
  - HTTP/3 (QUIC)，支持 QPACK 和 Server Push
  - 基于 URL 的自动协议选择（客户端）
- **Executor 框架** — 灵活的线程任务调度：
  - `executor` 抽象接口（类似 `boost::asio::executor`）
  - `thread_pool_executor` — 将任务分发到线程池
  - `strand` — 串行执行保证（不并发）
  - `co_spawn` / `run_on` — 在指定 executor 上启动协程
  - `work_guard` — 控制事件循环生命周期
  - `schedule_at_fixed_rate` — 周期性定时调度（类似 Java `ScheduledExecutorService`）
  - `sleep_for` / `sleep_until` — 协程异步睡眠
- **Spawn 与 JoinHandle** — 使用 `spawn()` 立即启动协程，通过 `JoinHandle<T>` 等待结果
- **零外部依赖** — 仅使用 C++ 标准库和操作系统 API（vcpkg 可选用于 SSL/QUIC）
- **可选 SSL/TLS** — 通过 OpenSSL / wolfSSL 提供内置 TLS 支持（`-DASYNC_NET_WITH_SSL=ON`）
- **TCP 和 UDP 支持** — 完整的 TCP 和 UDP 套接字异步操作
- **多播与广播** — 内置 UDP 多播/广播套接字选项支持
- **线程安全** — 所有 I/O 后端操作均使用互斥锁保护；跨线程 `post()` 自动唤醒
- **CMake 构建系统** — 易于集成到现有项目

## 项目结构

```
async_net/
├── CMakeLists.txt
├── include/async_net/
│   ├── coroutine/
│   │   ├── task.hpp              # Task<T> 协程类型
│   │   ├── spawn.hpp             # spawn() / JoinHandle<T>
│   │   └── async_result.hpp      # 回调到协程的桥接
│   ├── detail/
│   │   ├── config.hpp            # 平台检测与宏定义
│   │   ├── error_code.hpp        # 轻量级错误包装
│   │   ├── scope_guard.hpp       # RAII 作用域守卫
│   │   └── thread_pool.hpp       # 线程池实现
│   ├── executor/
│   │   ├── executor.hpp          # executor 接口与 any_executor
│   │   ├── thread_pool_executor.hpp  # thread_pool → executor 适配器
│   │   ├── strand.hpp            # 串行执行保证
│   │   ├── schedule.hpp          # sleep_for、scheduled_task
│   │   └── coroutine.hpp         # run_on、co_spawn、schedule_at_fixed_rate
│   ├── http/
│   │   ├── types.hpp             # HTTP 类型（request、response、method、headers）
│   │   ├── server.hpp            # HTTP 服务器（路由分发）
│   │   ├── client.hpp            # HTTP 客户端（连接池）
│   │   ├── handler.hpp           # 处理函数类型
│   │   ├── http2_session.hpp     # HTTP/2 会话（多路复用、Push）
│   │   └── http3_session.hpp     # HTTP/3 (QUIC) 会话
│   ├── io/
│   │   ├── io_backend.hpp        # 抽象 I/O 后端接口
│   │   ├── io_context.hpp        # 事件循环 (io_context) + executor
│   │   └── operation_context.hpp  # 每操作状态
│   └── net/
│       ├── buffer.hpp            # 缓冲区类型（mutable/const/dynamic）
│       ├── socket.hpp            # 基础套接字类
│       ├── tcp.hpp               # TCP 套接字与接受器
│       ├── ssl.hpp               # SSL/TLS 上下文与流
│       └── udp.hpp               # UDP 套接字与端点
├── src/
│   ├── io/
│   │   ├── io_context.cpp        # 事件循环实现
│   │   ├── epoll_backend.hpp/.cpp    # epoll 后端 (Linux)
│   │   ├── io_uring_backend.hpp/.cpp # io_uring 后端 (Linux 5.1+)
│   │   ├── kqueue_backend.hpp/.cpp   # kqueue 后端 (macOS/BSD)
│   │   ├── iocp_backend.hpp/.cpp     # IOCP 后端 (Windows)
│   │   └── ssl.cpp                 # SSL/TLS 实现
│   └── http/
│       ├── h1_codec.hpp          # HTTP/1.1 编解码器
│       ├── h2_frame.hpp          # HTTP/2 帧解析/构建
│       ├── h3_frame.hpp          # HTTP/3 帧解析/构建
│       ├── hpack.hpp             # HPACK 头部压缩
│       └── qpack.hpp             # QPACK 头部压缩
├── examples/
│   ├── echo_server.cpp           # 异步 Echo 服务器
│   ├── echo_client.cpp           # 异步 Echo 客户端
│   ├── echo_server_iouring.cpp   # 使用 io_uring 的 Echo 服务器
│   ├── multicast_sender.cpp      # 多播发送端
│   ├── multicast_receiver.cpp    # 多播接收端
│   ├── broadcast_sender.cpp      # 广播发送端
│   ├── broadcast_receiver.cpp    # 广播接收端
│   ├── ssl_echo_server.cpp       # TLS 回显服务器
│   ├── ssl_echo_client.cpp       # TLS 回显客户端
│   ├── http_server.cpp           # HTTP/1.1 服务器
│   ├── http_client.cpp           # HTTP 客户端
│   ├── http_multi_server.cpp     # 多协议服务器 (H1/H2/H3)
│   ├── http_multi_client.cpp     # 基于 URL 的多协议客户端
│   ├── h2_server.cpp             # HTTP/2 服务器（Server Push）
│   └── h3_server.cpp             # HTTP/3 (QUIC) 服务器
└── tests/
    ├── test_task.cpp             # Task<T> 单元测试
    ├── test_spawn.cpp            # spawn / JoinHandle 测试
    ├── test_executor.cpp         # Executor / strand / co_spawn 测试
    ├── test_h2_push.cpp          # HTTP/2 Server Push 测试
    ├── test_schedule.cpp         # 定时器 / schedule / sleep_for 测试
    ├── test_simple.cpp
    ├── test_nested.cpp
    └── test_void.cpp
```

## 环境要求

- **编译器**: GCC 10+（需 `-fcoroutines`）、Clang 14+ 或 MSVC 19.28+
- **CMake**: 3.20+
- **C++ 标准**: C++20

## 构建

### 使用 vcpkg 构建（推荐 — 启用 SSL、HTTP/2、HTTP/3）

项目使用 [vcpkg](https://github.com/microsoft/vcpkg) manifest 模式管理可选依赖（wolfSSL、ngtcp2）。使用 vcpkg 时，SSL/TLS、HTTP/2 和 HTTP/3 会自动检测并启用。

```bash
# 1. 安装 vcpkg（一次性操作）
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
cd ..

# 2. 配置 — 传入 vcpkg 工具链文件
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
         -DCMAKE_BUILD_TYPE=Release

# 3. 构建（vcpkg 自动下载并编译 wolfssl[quic] + ngtcp2[wolfssl]）
cmake --build . -j$(nproc)

# 4. 运行测试
ctest --output-on-failure
```

> **工作原理**：`vcpkg.json` manifest 声明了 `wolfssl[quic]` 和 `ngtcp2[wolfssl]` 依赖。CMake 的 `find_package()` 调用在配置阶段由 vcpkg 自动解析，无需手动安装任何库。

**依赖关系：**

```
async_net
├── wolfssl[quic]        → TLS 1.3 + QUIC 支持
└── ngtcp2[wolfssl]      → QUIC 协议（使用 wolfSSL 作为加密后端）
```

### 不使用 vcpkg 构建（基础模式 — 无 SSL/HTTP2/HTTP3）

不使用 vcpkg 时，库仅构建基础 TCP/UDP 支持。SSL、HTTP/2 和 HTTP/3 会自动禁用。

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### macOS

需要安装 **Xcode 命令行工具**：

```bash
xcode-select --install   # 一次性设置
```

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
```

> **注意**：macOS 自动使用 **kqueue** 后端。
> 不支持从 Linux 交叉编译到 macOS — 需要 macOS 机器（或 CI 运行器）。

### Windows (MSVC)

需要安装 **Visual Studio 2022**（17.8+）并勾选 C++ 工作负载。

```powershell
# 在 Developer PowerShell 或 CMD（cl.exe 已在 PATH 中）执行
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

或使用 Ninja（推荐，构建更快）：

```powershell
# 确保 Ninja 和 cl.exe 在 PATH 中
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

可执行文件输出位置：

```
build/Release/echo_server.exe
build/Release/echo_client.exe
```

> **注意**：MSVC 需要 `/std:c++20`，CMake 会通过 `CMAKE_CXX_STANDARD 20` 自动设置。
> 如果协程相关代码无法编译，请确保使用 VS 2022 17.8+（MSVC 19.38+）。

### Windows (MinGW-w64)

需要 **MinGW-w64** 且 GCC 10+（支持 C++20 协程）。

```bash
# 使用 MSYS2 MinGW-w64 环境
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

或使用 Makefile：

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

可执行文件输出位置：

```
build/echo_server.exe
build/echo_client.exe
```

> **注意**：MinGW 使用 GCC 的 `-fcoroutines` 标志，CMakeLists.txt 会自动设置。
> 库会链接 `ws2_32` 和 `mswsock` 以支持 Winsock。
> `WSAStartup` 在静态初始化时自动调用。

### 从 Linux 交叉编译 Windows 版本 (MinGW-w64)

安装交叉编译器：

```bash
# Ubuntu / Debian
sudo apt install mingw-w64
```

使用提供的工具链文件构建：

```bash
mkdir build-mingw && cd build-mingw
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

无需离开 Linux 即可生成 Windows `.exe` 文件。

### 运行测试

```bash
cd build
ctest --output-on-failure
```

### 运行示例

启动 echo 服务器：

```bash
./build/echo_server              # Linux/macOS
.\build\Release\echo_server.exe  # Windows
```

在另一个终端运行 echo 客户端：

```bash
./build/echo_client              # Linux/macOS
.\build\Release\echo_client.exe  # Windows
```

## 快速开始

### Echo 服务器

```cpp
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <cstdio>
#include <cstring>

using namespace async_net;

Task<void> handle_client(tcp::socket sock) {
    char buf[1024];
    while (true) {
        auto n = co_await sock.async_read_some(buffer(buf, sizeof(buf)));
        if (n <= 0) break;
        co_await sock.async_write_some(const_buffer(buf, n));
    }
    co_return;
}

Task<void> run_server(io_context& ctx, uint16_t port) {
    tcp::acceptor acc(ctx, port);
    while (true) {
        auto sock = co_await acc.async_accept();
        handle_client(std::move(sock)).resume();
    }
    co_return;
}

int main() {
    io_context ctx;
    run_server(ctx, 8080).resume();
    ctx.run();
    return 0;
}
```

### 异步客户端

```cpp
#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/tcp.hpp>
#include <cstdio>

using namespace async_net;

Task<void> run_client(io_context& ctx) {
    tcp::socket sock(ctx);
    co_await sock.async_connect("127.0.0.1", 8080);

    const char* msg = "Hello!";
    co_await sock.async_write_some(const_buffer(msg, strlen(msg)));

    char buf[1024];
    auto n = co_await sock.async_read_some(buffer(buf, sizeof(buf)));
    printf("Received: %.*s\n", (int)n, buf);

    co_return;
}

int main() {
    io_context ctx;
    run_client(ctx).resume();
    ctx.run();
    return 0;
}
```

## 架构设计

```
┌──────────────────────────────────────────────────────────────┐
│                  用户代码（协程）                              │
│   co_await socket.async_read()  │  co_await client.get(url) │
├──────────────────────────────────────────────────────────────┤
│              HTTP 协议栈                                      │
│   HTTP/1.1 (keep-alive) │ HTTP/2 (多路复用) │ HTTP/3 (QUIC) │
├──────────────────────────────────────────────────────────────┤
│         Executor 框架                                         │
│   io_context │ thread_pool_executor │ strand │ co_spawn      │
├──────────────────────────────────────────────────────────────┤
│         协程层 (Task<T>、JoinHandle<T>、spawn)               │
│    Awaiter 桥接协程 ↔ I/O 后端                               │
├──────────────────────────────────────────────────────────────┤
│              io_context（事件循环 + executor）                │
│         poll() → 恢复协程 │ post() → wake()                 │
├──────────────────────────────────────────────────────────────┤
│           I/O 后端（抽象接口）                                │
│   epoll │ io_uring │ kqueue │ IOCP                           │
├──────────────────────────────────────────────────────────────┤
│               操作系统 I/O API                                │
│   epoll_wait │ kevent │ GetQueuedCompletionStatus            │
└──────────────────────────────────────────────────────────────┘
```

### 关键设计决策

1. **惰性协程** — `Task<T>` 是惰性协程，不会自动启动，需要调用 `.resume()` 或被另一个协程 `co_await`。
2. **即时启动** — `spawn()` 立即启动协程，返回 `JoinHandle<T>` 用于等待结果。
3. **同步完成检测** — Awaiter 检查后端是否同步完成了操作（如数据已就绪），如果已完成则避免挂起协程。
4. **锁外恢复** — 事件循环在持有锁时收集已完成的操作，释放锁后再恢复协程，防止死锁。
5. **句柄所有权** — 当协程被 `co_await` 时，句柄所有权通过 `std::exchange` 转移，防止重复销毁。
6. **Executor 抽象** — `io_context` 继承 `executor`，支持在 IO 线程、线程池和 strand 上统一调度任务。

## API 参考

### `io_context`

驱动所有异步操作的事件循环，同时也是一个 `executor`。

```cpp
io_context ctx;
ctx.run();               // 运行直到停止
ctx.run_until_complete(); // 运行直到所有 work_guard 释放
ctx.run_one();           // 运行一个操作
ctx.poll();              // 非阻塞轮询
ctx.stop();              // 停止事件循环
ctx.post([]{ ... });     // 投递任务（线程安全，唤醒阻塞的 poll）

// work_guard：防止 run_until_complete() 提前退出
auto guard = ctx.make_work();
// ... 执行异步工作 ...
guard.reset();           // 释放 work，允许退出
```

### `tcp::socket`

支持协程的异步 TCP 套接字。

```cpp
tcp::socket sock(ctx);
co_await sock.async_connect("127.0.0.1", 8080);
auto n = co_await sock.async_read_some(buffer(buf, sizeof(buf)));
co_await sock.async_write_some(const_buffer(data, len));
co_await sock.async_write(const_buffer(data, len)); // 写入所有字节
```

### `tcp::acceptor`

用于服务器的异步 TCP 接受器。

```cpp
tcp::acceptor acc(ctx, 8080);
auto client = co_await acc.async_accept();
```

### `udp::socket`

异步 UDP 套接字。

```cpp
udp::socket sock(ctx);
auto [n, ep] = co_await sock.async_receive_from(buffer(buf, sizeof(buf)));
co_await sock.async_send_to(const_buffer(data, len), ep);
```

### `Task<T>`

具有所有权语义的惰性协程返回类型。

```cpp
Task<int> compute() {
    co_return 42;
}

Task<void> consumer() {
    int val = co_await compute();
    co_return;
}
```

### `spawn()` 与 `JoinHandle<T>`

立即启动协程，可选等待其结果。

```cpp
// Fire-and-forget
spawn(my_task()).detach();

// 等待结果
auto handle = spawn(compute());
int result = co_await std::move(handle);
```

### Executor 框架

使用 executor 抽象跨线程调度任务。

```cpp
// 线程池 executor
thread_pool pool(4);
thread_pool_executor pool_exec(pool);
pool_exec.post([]{ /* 在线程池线程上运行 */ });

// strand：串行执行（不并发）
strand s(ctx);
s.post([]{ /* 串行运行 */ });
s.post([]{ /* 在前一个完成后运行 */ });

// run_on：在协程中切换执行上下文
co_await run_on(pool_exec);   // 切换到线程池
// ... CPU 密集型工作 ...
co_await run_on(ctx);         // 切换回 io_context

// co_spawn：在指定 executor 上启动协程
auto handle = co_spawn(pool_exec, compute());
int result = co_await std::move(handle);

// detach：在指定 executor 上 fire-and-forget
detach(pool_exec, my_task());
```

### 定时调度

周期性和延迟任务调度（类似 Java `ScheduledExecutorService`）。

```cpp
// sleep_for：协程中异步睡眠
co_await sleep_for(std::chrono::seconds(1));
co_await sleep_for(500ms, ctx);  // 显式指定 io_context

// schedule_once：延迟执行任务（可取消）
auto task = schedule_once(ctx, 500ms, my_task());
task.cancel();  // 在触发前取消

// schedule_at_fixed_rate：周期性执行（可取消）
auto t = schedule_at_fixed_rate(ctx, 1s,
    []{ return my_task(); }, 2s);  // 工厂函数：每次迭代创建新 Task
t.cancel();  // 停止周期执行
```

### HTTP 服务器

基于路由的 HTTP 服务器，支持 H1/H2/H3。

```cpp
http::server srv(ctx, 8080);

srv.route(http::method::GET, "/hello", [](const http::request& req) -> Task<http::response> {
    co_return http::response{200, "Hello, World!"};
});

// H2/H3 Server Push
srv.set_push_provider([](const http::request& req) {
    return std::vector<std::pair<http::request, http::response>>{
        {{http::method::GET, "/style.css"}, {200, "body{}"}}
    };
});

co_await srv.serve();
// co_await srv.serve_h2(ssl_ctx);  // HTTP/2 + TLS
```

### HTTP 客户端

基于 URL 的 HTTP 客户端，自动选择协议，支持连接池。

```cpp
http::client cli(ctx);
auto resp = co_await cli.get("https://example.com/api/data");
printf("Status: %d, Body: %s\n", resp.status_code, resp.body.c_str());

// POST 请求
auto resp2 = co_await cli.post("https://example.com/api/submit", "{\"key\":\"value\"}", "application/json");
```

## 平台支持

| 平台     | 后端       | 状态        |
|----------|------------|-------------|
| Linux    | epoll      | ✅ 默认     |
| Linux    | io_uring   | ✅ 可选     |
| macOS    | kqueue     | ✅ 完整     |
| FreeBSD  | kqueue     | ✅ 完整     |
| Windows  | IOCP       | ✅ 完整     |

## 许可证

MIT 许可证。详见 [LICENSE](LICENSE) 文件。
