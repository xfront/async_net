#pragma once

#include <async_net/http/types.hpp>
#include <async_net/coroutine/task.hpp>
#include <concepts>
#include <functional>

namespace async_net::http {

// Handler type: any callable that takes a request and returns Task<response>
using handler_fn = std::function<Task<response>(const request&)>;

// Handler concept for generic handlers
template<typename F>
concept HttpHandler = requires(F f, const request& req) {
    { f(req) } -> std::same_as<Task<response>>;
};

} // namespace async_net::http
