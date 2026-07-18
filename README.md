# async_net

A high-performance, header-friendly C++20 coroutine-based asynchronous network library with cross-platform I/O backend support.

## Features

- **C++20 Coroutines** — Write asynchronous code that reads like synchronous code using `co_await` / `co_return`
- **Multiple I/O Backends** — Automatically selects the best backend for your platform:
  - **epoll** — Linux (default)
  - **io_uring** — Linux 5.1+ (pure async, opt-in)
  - **kqueue** — macOS / FreeBSD / OpenBSD
  - **IOCP** — Windows
- **HTTP Protocol Stack** — Full HTTP/1.1, HTTP/2, and HTTP/3 support:
  - HTTP/1.1 with keep-alive and connection pooling
  - HTTP/2 with multiplexing, HPACK, and Server Push
  - HTTP/3 (QUIC) with QPACK and Server Push
  - URL-based auto protocol selection (client)
- **Executor Framework** — Flexible task scheduling and threading:
  - `executor` abstract interface (similar to `boost::asio::executor`)
  - `thread_pool_executor` — dispatch work to a thread pool
  - `strand` — serialized execution guarantee (no concurrent execution)
  - `co_spawn` / `run_on` — launch coroutines on specific executors
  - `work_guard` — control event loop lifetime
  - `schedule_at_fixed_rate` — periodic task scheduling (like Java `ScheduledExecutorService`)
  - `sleep_for` / `sleep_until` — coroutine-friendly async sleep
- **Spawn & JoinHandle** — Eagerly start coroutines with `spawn()`, await results via `JoinHandle<T>`
- **Zero External Dependencies** — Only uses the C++ standard library and OS APIs (vcpkg optional for SSL/QUIC)
- **Optional SSL/TLS** — Built-in TLS support via OpenSSL / wolfSSL (`-DASYNC_NET_WITH_SSL=ON`)
- **TCP & UDP Support** — Full async operations for both TCP and UDP sockets
- **Multicast & Broadcast** — Built-in socket options for UDP multicast/broadcast
- **Thread-Safe** — All I/O backend operations are protected by mutexes; cross-thread `post()` with wake-up
- **CMake Build System** — Easy to integrate into existing projects

## Project Structure

```
async_net/
├── CMakeLists.txt
├── include/async_net/
│   ├── coroutine/
│   │   ├── task.hpp              # Task<T> coroutine type
│   │   ├── spawn.hpp             # spawn() / JoinHandle<T>
│   │   └── async_result.hpp      # Callback-to-coroutine bridge
│   ├── detail/
│   │   ├── config.hpp            # Platform detection & macros
│   │   ├── error_code.hpp        # Lightweight error wrapper
│   │   ├── scope_guard.hpp       # RAII scope guard
│   │   └── thread_pool.hpp       # Thread pool implementation
│   ├── executor/
│   │   ├── executor.hpp          # executor interface & any_executor
│   │   ├── thread_pool_executor.hpp  # thread_pool → executor adapter
│   │   ├── strand.hpp            # Serialized execution guarantee
│   │   ├── schedule.hpp          # sleep_for, scheduled_task
│   │   └── coroutine.hpp         # run_on, co_spawn, schedule_at_fixed_rate
│   ├── http/
│   │   ├── types.hpp             # HTTP types (request, response, method, headers)
│   │   ├── server.hpp            # HTTP server with routing
│   │   ├── client.hpp            # HTTP client with connection pooling
│   │   ├── handler.hpp           # Handler function type
│   │   ├── http2_session.hpp     # HTTP/2 session (multiplexing, push)
│   │   └── http3_session.hpp     # HTTP/3 (QUIC) session
│   ├── io/
│   │   ├── io_backend.hpp        # Abstract I/O backend interface
│   │   ├── io_context.hpp        # Event loop (io_context) + executor
│   │   └── operation_context.hpp  # Per-operation state
│   └── net/
│       ├── buffer.hpp            # Buffer types (mutable/const/dynamic)
│       ├── socket.hpp            # Base socket class
│       ├── ssl.hpp               # SSL/TLS context & stream
│       ├── tcp.hpp               # TCP socket & acceptor
│       └── udp.hpp               # UDP socket & endpoint
├── src/
│   ├── io/
│   │   ├── io_context.cpp        # Event loop implementation
│   │   ├── epoll_backend.hpp/.cpp    # epoll backend (Linux)
│   │   ├── io_uring_backend.hpp/.cpp # io_uring backend (Linux 5.1+)
│   │   ├── kqueue_backend.hpp/.cpp   # kqueue backend (macOS/BSD)
│   │   ├── iocp_backend.hpp/.cpp     # IOCP backend (Windows)
│   │   └── ssl.cpp                 # SSL/TLS implementation
│   └── http/
│       ├── h1_codec.hpp          # HTTP/1.1 codec
│       ├── h2_frame.hpp          # HTTP/2 frame parser/builder
│       ├── h3_frame.hpp          # HTTP/3 frame parser/builder
│       ├── hpack.hpp             # HPACK header compression
│       └── qpack.hpp             # QPACK header compression
├── examples/
│   ├── echo_server.cpp           # Async echo server
│   ├── echo_client.cpp           # Async echo client
│   ├── echo_server_iouring.cpp   # Echo server using io_uring
│   ├── multicast_sender.cpp      # Multicast sender
│   ├── multicast_receiver.cpp    # Multicast receiver
│   ├── broadcast_sender.cpp      # Broadcast sender
│   ├── broadcast_receiver.cpp    # Broadcast receiver
│   ├── ssl_echo_server.cpp       # TLS echo server
│   ├── ssl_echo_client.cpp       # TLS echo client
│   ├── http_server.cpp           # HTTP/1.1 server
│   ├── http_client.cpp           # HTTP client
│   ├── http_multi_server.cpp     # Multi-protocol server (H1/H2/H3)
│   ├── http_multi_client.cpp     # URL-based multi-protocol client
│   ├── h2_server.cpp             # HTTP/2 server with Server Push
│   └── h3_server.cpp             # HTTP/3 (QUIC) server
└── tests/
    ├── test_task.cpp             # Task<T> unit tests
    ├── test_spawn.cpp            # spawn / JoinHandle tests
    ├── test_executor.cpp         # Executor / strand / co_spawn tests
    ├── test_h2_push.cpp          # HTTP/2 Server Push tests
    ├── test_schedule.cpp         # Timer / schedule / sleep_for tests
    ├── test_simple.cpp
    ├── test_nested.cpp
    └── test_void.cpp
```

## Requirements

- **Compiler**: GCC 10+ (with `-fcoroutines`), Clang 14+, or MSVC 19.28+
- **CMake**: 3.20+
- **C++ Standard**: C++20

## Building

### Build with vcpkg (Recommended — enables SSL, HTTP/2, HTTP/3)

The project uses [vcpkg](https://github.com/microsoft/vcpkg) manifest mode to manage optional dependencies (wolfSSL, ngtcp2). With vcpkg, SSL/TLS, HTTP/2, and HTTP/3 are automatically detected and enabled.

```bash
# 1. Install vcpkg (one-time)
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
cd ..

# 2. Configure — pass the vcpkg toolchain file
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
         -DCMAKE_BUILD_TYPE=Release

# 3. Build (vcpkg automatically downloads and builds wolfssl[quic] + ngtcp2[wolfssl])
cmake --build . -j$(nproc)

# 4. Run tests
ctest --output-on-failure
```

> **How it works**: The `vcpkg.json` manifest declares dependencies on `wolfssl[quic]` and `ngtcp2[wolfssl]`. CMake's `find_package()` calls are resolved by vcpkg automatically during configuration. No manual library installation needed.

**Dependency graph:**

```
async_net
├── wolfssl[quic]        → TLS 1.3 + QUIC support
└── ngtcp2[wolfssl]      → QUIC protocol (uses wolfSSL for crypto)
```

### Build without vcpkg (basic mode — no SSL/HTTP2/HTTP3)

Without vcpkg, the library builds with basic TCP/UDP support only. SSL, HTTP/2, and HTTP/3 are automatically disabled.

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

Requires **Xcode Command Line Tools**:

```bash
xcode-select --install   # one-time setup
```

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
```

> **Note**: macOS uses the **kqueue** backend automatically.
> Cross-compiling for macOS from Linux is not supported — a macOS machine (or CI runner) is required.

### Windows (MSVC)

Requires **Visual Studio 2022** (17.8+) with the C++ workload installed.

```powershell
# Developer PowerShell or CMD with cl.exe in PATH
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Or using Ninja (recommended for faster builds):

```powershell
# Make sure Ninja and cl.exe are in PATH
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Executable output:

```
build/Release/echo_server.exe
build/Release/echo_client.exe
```

> **Note**: MSVC requires `/std:c++20` which CMake sets automatically via `CMAKE_CXX_STANDARD 20`.
> If coroutines don't compile, ensure you're on VS 2022 17.8+ (MSVC 19.38+).

### Windows (MinGW-w64)

Requires **MinGW-w64** with GCC 10+ (for C++20 coroutine support).

```bash
# Using MSYS2 MinGW-w64 environment
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Or using Makefiles:

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Executable output:

```
build/echo_server.exe
build/echo_client.exe
```

> **Note**: MinGW uses GCC's `-fcoroutines` flag, which CMakeLists.txt sets automatically.
> The library links `ws2_32` and `mswsock` for Winsock support.
> `WSAStartup` is called automatically during static initialization.

### Cross-compile for Windows from Linux (MinGW-w64)

Install the cross-compiler:

```bash
# Ubuntu / Debian
sudo apt install mingw-w64
```

Build using the provided toolchain file:

```bash
mkdir build-mingw && cd build-mingw
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

This produces Windows `.exe` files without leaving Linux.

### Running Tests

```bash
cd build
ctest --output-on-failure
```

### Running Examples

Start the echo server:

```bash
./build/echo_server          # Linux/macOS
.\build\Release\echo_server.exe  # Windows
```

In another terminal, run the echo client:

```bash
./build/echo_client          # Linux/macOS
.\build\Release\echo_client.exe  # Windows
```

## Quick Start

### Echo Server

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

### Async Client

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

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                  User Code (coroutines)                       │
│   co_await socket.async_read()  │  co_await client.get(url) │
├──────────────────────────────────────────────────────────────┤
│              HTTP Protocol Stack                              │
│   HTTP/1.1 (keep-alive) │ HTTP/2 (multiplex) │ HTTP/3 (QUIC)│
├──────────────────────────────────────────────────────────────┤
│         Executor Framework                                    │
│   io_context │ thread_pool_executor │ strand │ co_spawn      │
├──────────────────────────────────────────────────────────────┤
│         Coroutine Layer (Task<T>, JoinHandle<T>, spawn)      │
│    Awaiters bridge coroutines ↔ I/O backend                  │
├──────────────────────────────────────────────────────────────┤
│              io_context (event loop + executor)               │
│         poll() → resume coroutines │ post() → wake()         │
├──────────────────────────────────────────────────────────────┤
│           I/O Backend (abstract)                              │
│   epoll │ io_uring │ kqueue │ IOCP                           │
├──────────────────────────────────────────────────────────────┤
│               OS I/O APIs                                    │
│   epoll_wait │ kevent │ GetQueuedCompletionStatus            │
└──────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **Lazy Coroutines** — `Task<T>` is a lazy coroutine; it does not start until `.resume()` is called or it is `co_await`ed by another coroutine.
2. **Eager Spawn** — `spawn()` starts a coroutine immediately and returns a `JoinHandle<T>` that can be `co_await`ed for the result.
3. **Synchronous Completion** — Awaiters check if the backend completed the operation synchronously (e.g., data already available) and avoid suspending the coroutine if so.
4. **Resume Outside Lock** — The event loop collects completed operations under the lock, then resumes coroutines after releasing it, preventing deadlocks.
5. **Handle Ownership** — When a coroutine is `co_await`ed, handle ownership is transferred via `std::exchange` to prevent double-destroy.
6. **Executor Abstraction** — `io_context` inherits `executor`, enabling uniform work scheduling across io threads, thread pools, and strands.

## API Reference

### `io_context`

The event loop that drives all async operations. Also acts as an `executor`.

```cpp
io_context ctx;
ctx.run();               // Run until stopped
ctx.run_until_complete(); // Run until all work_guard released
ctx.run_one();           // Run one operation
ctx.poll();              // Poll without blocking
ctx.stop();              // Stop the event loop
ctx.post([]{ ... });     // Post work (thread-safe, wakes blocked poll)

// work_guard: prevents run_until_complete() from exiting
auto guard = ctx.make_work();
// ... do async work ...
guard.reset();           // release work, allow exit
```

### `tcp::socket`

Async TCP socket with coroutine support.

```cpp
tcp::socket sock(ctx);
co_await sock.async_connect("127.0.0.1", 8080);
auto n = co_await sock.async_read_some(buffer(buf, sizeof(buf)));
co_await sock.async_write_some(const_buffer(data, len));
co_await sock.async_write(const_buffer(data, len)); // writes all bytes
```

### `tcp::acceptor`

Async TCP acceptor for servers.

```cpp
tcp::acceptor acc(ctx, 8080);
auto client = co_await acc.async_accept();
```

### `udp::socket`

Async UDP socket.

```cpp
udp::socket sock(ctx);
auto [n, ep] = co_await sock.async_receive_from(buffer(buf, sizeof(buf)));
co_await sock.async_send_to(const_buffer(data, len), ep);
```

### `Task<T>`

Lazy coroutine return type with ownership semantics.

```cpp
Task<int> compute() {
    co_return 42;
}

Task<void> consumer() {
    int val = co_await compute();
    co_return;
}
```

### `spawn()` & `JoinHandle<T>`

Eagerly start a coroutine and optionally await its result.

```cpp
// Fire-and-forget
spawn(my_task()).detach();

// Await result
auto handle = spawn(compute());
int result = co_await std::move(handle);
```

### Executor Framework

Schedule work across threads with executor abstractions.

```cpp
// Thread pool executor
thread_pool pool(4);
thread_pool_executor pool_exec(pool);
pool_exec.post([]{ /* runs on pool thread */ });

// strand: serialized execution (no concurrency)
strand s(ctx);
s.post([]{ /* runs serially */ });
s.post([]{ /* runs after previous completes */ });

// run_on: switch execution context in a coroutine
co_await run_on(pool_exec);   // switch to pool thread
// ... do CPU-bound work ...
co_await run_on(ctx);         // switch back to io_context

// co_spawn: launch a coroutine on a specific executor
auto handle = co_spawn(pool_exec, compute());
int result = co_await std::move(handle);

// detach: fire-and-forget on a specific executor
detach(pool_exec, my_task());
```

### Scheduled Execution

Periodic and delayed task scheduling (like Java `ScheduledExecutorService`).

```cpp
// sleep_for: async sleep in a coroutine
co_await sleep_for(std::chrono::seconds(1));
co_await sleep_for(500ms, ctx);  // explicit io_context

// schedule_once: run a task after a delay (cancellable)
auto task = schedule_once(ctx, 500ms, my_task());
task.cancel();  // cancel before it fires

// schedule_at_fixed_rate: periodic execution (cancellable)
auto t = schedule_at_fixed_rate(ctx, 1s,
    []{ return my_task(); }, 2s);  // factory: new Task each iteration
t.cancel();  // stops periodic execution
```

### HTTP Server

Route-based HTTP server with H1/H2/H3 support.

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
// co_await srv.serve_h2(ssl_ctx);  // HTTP/2 with TLS
```

### HTTP Client

URL-based HTTP client with automatic protocol selection and connection pooling.

```cpp
http::client cli(ctx);
auto resp = co_await cli.get("https://example.com/api/data");
printf("Status: %d, Body: %s\n", resp.status_code, resp.body.c_str());

// POST request
auto resp2 = co_await cli.post("https://example.com/api/submit", "{\"key\":\"value\"}", "application/json");
```

## Platform Support

| Platform | Backend    | Status      |
|----------|------------|-------------|
| Linux    | epoll      | ✅ Default  |
| Linux    | io_uring   | ✅ Opt-in   |
| macOS    | kqueue     | ✅ Full     |
| FreeBSD  | kqueue     | ✅ Full     |
| Windows  | IOCP       | ✅ Full     |

## License

MIT License. See [LICENSE](LICENSE) for details.
