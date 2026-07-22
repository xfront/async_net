#pragma once

// Fully async DNS resolver
// Primary: pure async UDP + coroutines (no blocking)
// Fallback: thread-based getaddrinfo (for compatibility with strict DNS servers)
//
// Usage:
//   auto result = co_await async_resolve(ctx, "example.com", 8080);
//   if (result.error == 0) {
//       // result.addr contains the resolved sockaddr_in
//   }

#include "../coroutine/task.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdint>
#include <string>

namespace async_net { class io_context; }

namespace async_net::net {

// DNS resolution result
struct resolve_result {
    struct sockaddr_in addr{};
    int error = -1;  // 0 = success
};

// Async DNS resolution (fully async, no blocking)
// Tries pure async UDP DNS first, falls back to thread-based getaddrinfo
Task<resolve_result> async_resolve(async_net::io_context& ctx, const std::string& host, uint16_t port);

} // namespace async_net::net
