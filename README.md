# async_net

A high-performance, header-friendly C++20 coroutine-based asynchronous network library with cross-platform I/O backend support.

## Features

- **C++20 Coroutines** — Write asynchronous code that reads like synchronous code using `co_await` / `co_return`
- **Multiple I/O Backends** — Automatically selects the best backend for your platform:
  - **epoll** — Linux
  - **kqueue** — macOS / FreeBSD / OpenBSD
  - **IOCP** — Windows
  - **Linux AIO** — Linux (hybrid implementation)
- **Zero External Dependencies** — Only uses the C++ standard library and OS APIs
- **TCP & UDP Support** — Full async operations for both TCP and UDP sockets
- **Thread-Safe** — All I/O backend operations are protected by mutexes
- **CMake Build System** — Easy to integrate into existing projects

## Project Structure

```
async_net/
├── CMakeLists.txt
├── include/async_net/
│   ├── coroutine/
│   │   ├── task.hpp              # Task<T> coroutine type
│   │   └── async_result.hpp      # Callback-to-coroutine bridge
│   ├── detail/
│   │   ├── config.hpp            # Platform detection & macros
│   │   ├── error_code.hpp        # Lightweight error wrapper
│   │   ├── scope_guard.hpp       # RAII scope guard
│   │   └── thread_pool.hpp       # Thread pool implementation
│   ├── io/
│   │   ├── io_backend.hpp        # Abstract I/O backend interface
│   │   ├── io_context.hpp        # Event loop (io_context)
│   │   └── operation_context.hpp  # Per-operation state
│   └── net/
│       ├── buffer.hpp            # Buffer types (mutable/const/dynamic)
│       ├── socket.hpp            # Base socket class
│       ├── tcp.hpp               # TCP socket & acceptor
│       └── udp.hpp               # UDP socket & endpoint
├── src/io/
│   ├── io_context.cpp            # Event loop implementation
│   ├── epoll_backend.hpp/.cpp    # epoll backend (Linux)
│   ├── kqueue_backend.hpp/.cpp   # kqueue backend (macOS/BSD)
│   ├── iocp_backend.hpp/.cpp     # IOCP backend (Windows)
│   └── aio_backend.hpp/.cpp      # Linux AIO backend
├── examples/
│   ├── echo_server.cpp           # Async echo server
│   └── echo_client.cpp           # Async echo client
└── tests/
    ├── test_task.cpp             # Task<T> unit tests
    ├── test_simple.cpp
    ├── test_nested.cpp
    └── test_void.cpp
```

## Requirements

- **Compiler**: GCC 10+ (with `-fcoroutines`), Clang 14+, or MSVC 19.28+
- **CMake**: 3.20+
- **C++ Standard**: C++20

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Running Tests

```bash
cd build
ctest --output-on-failure
```

### Running Examples

Start the echo server:

```bash
./build/echo_server
```

In another terminal, run the echo client:

```bash
./build/echo_client
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
┌─────────────────────────────────────────────┐
│            User Code (coroutines)           │
│         co_await socket.async_read_some()   │
├─────────────────────────────────────────────┤
│         Coroutine Layer (Task<T>)           │
│    Awaiters bridge coroutines ↔ I/O backend │
├─────────────────────────────────────────────┤
│              io_context (event loop)        │
│         poll() → resume coroutines          │
├─────────────────────────────────────────────┤
│           I/O Backend (abstract)            │
│   epoll │ kqueue │ IOCP │ Linux AIO         │
├─────────────────────────────────────────────┤
│               OS I/O APIs                   │
│   epoll_wait │ kevent │ GetQueuedCompletionStatus │
└─────────────────────────────────────────────┘
```

### Key Design Decisions

1. **Lazy Coroutines** — `Task<T>` is a lazy coroutine; it does not start until `.resume()` is called or it is `co_await`ed by another coroutine.
2. **Synchronous Completion** — Awaiters check if the backend completed the operation synchronously (e.g., data already available) and avoid suspending the coroutine if so.
3. **Resume Outside Lock** — The event loop collects completed operations under the lock, then resumes coroutines after releasing it, preventing deadlocks.
4. **Handle Ownership** — When a coroutine is `co_await`ed, handle ownership is transferred via `std::exchange` to prevent double-destroy.

## API Reference

### `io_context`

The event loop that drives all async operations.

```cpp
io_context ctx;
ctx.run();       // Run until stopped
ctx.run_one();   // Run one operation
ctx.poll();      // Poll without blocking
ctx.stop();      // Stop the event loop
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

## Platform Support

| Platform | Backend    | Status      |
|----------|------------|-------------|
| Linux    | epoll      | ✅ Full     |
| Linux    | AIO        | ✅ Hybrid   |
| macOS    | kqueue     | ✅ Full     |
| FreeBSD  | kqueue     | ✅ Full     |
| Windows  | IOCP       | ✅ Full     |

## License

MIT License. See [LICENSE](LICENSE) for details.
