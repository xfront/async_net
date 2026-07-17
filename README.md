# async_net

A high-performance, header-friendly C++20 coroutine-based asynchronous network library with cross-platform I/O backend support.

## Features

- **C++20 Coroutines** — Write asynchronous code that reads like synchronous code using `co_await` / `co_return`
- **Multiple I/O Backends** — Automatically selects the best backend for your platform:
  - **epoll** — Linux (default)
  - **io_uring** — Linux 5.1+ (pure async, opt-in)
  - **kqueue** — macOS / FreeBSD / OpenBSD
  - **IOCP** — Windows
- **Zero External Dependencies** — Only uses the C++ standard library and OS APIs
- **Optional SSL/TLS** — Built-in TLS support via OpenSSL (`-DASYNC_NET_WITH_SSL=ON`)
- **TCP & UDP Support** — Full async operations for both TCP and UDP sockets
- **Multicast & Broadcast** — Built-in socket options for UDP multicast/broadcast
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
│       ├── ssl.hpp               # SSL/TLS context & stream (requires OpenSSL)
│       ├── tcp.hpp               # TCP socket & acceptor
│       └── udp.hpp               # UDP socket & endpoint
├── src/io/
│   ├── io_context.cpp            # Event loop implementation
│   ├── epoll_backend.hpp/.cpp    # epoll backend (Linux)
│   ├── io_uring_backend.hpp/.cpp # io_uring backend (Linux 5.1+)
│   ├── kqueue_backend.hpp/.cpp   # kqueue backend (macOS/BSD)
│   └── iocp_backend.hpp/.cpp     # IOCP backend (Windows)
│   └── ssl.cpp                   # SSL/TLS implementation (OpenSSL)
├── examples/
│   ├── echo_server.cpp           # Async echo server
│   ├── echo_client.cpp           # Async echo client
│   ├── echo_server_iouring.cpp   # Echo server using io_uring
│   ├── multicast_sender.cpp      # Multicast sender
│   ├── multicast_receiver.cpp    # Multicast receiver
│   ├── broadcast_sender.cpp      # Broadcast sender
│   ├── broadcast_receiver.cpp    # Broadcast receiver
│   ├── ssl_echo_server.cpp       # TLS echo server (requires OpenSSL)
│   └── ssl_echo_client.cpp       # TLS echo client (requires OpenSSL)
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
│   epoll │ io_uring │ kqueue │ IOCP          │
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
| Linux    | epoll      | ✅ Default  |
| Linux    | io_uring   | ✅ Opt-in   |
| macOS    | kqueue     | ✅ Full     |
| FreeBSD  | kqueue     | ✅ Full     |
| Windows  | IOCP       | ✅ Full     |

## License

MIT License. See [LICENSE](LICENSE) for details.
