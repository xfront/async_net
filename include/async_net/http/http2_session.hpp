// HTTP/2 Session — zero-dependency implementation (no nghttp2)
// Wraps frame parsing, HPACK, stream management, and flow control.
#pragma once

#include <async_net/http/types.hpp>
#include <async_net/http/handler.hpp>
#include <async_net/coroutine/task.hpp>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace async_net::http {

class http2_session {
public:
    using send_fn = std::function<Task<ssize_t>(const uint8_t*, size_t)>;
    using request_handler = std::function<response(const request&)>;

    // Push promise info — delivered to client when PUSH_PROMISE is received
    struct push_promise_info {
        int32_t promised_stream_id = 0;
        request promised_request;
    };

    // Server: callback invoked after responding to a request, returns pushes to send
    using push_provider = std::function<std::vector<std::pair<request, response>>(const request&)>;

    // Client: callback when PUSH_PROMISE is received
    using push_handler = std::function<void(const push_promise_info&)>;

    enum class mode { server, client };

    http2_session(mode m);
    ~http2_session();

    http2_session(const http2_session&) = delete;
    http2_session& operator=(const http2_session&) = delete;
    http2_session(http2_session&&) noexcept;
    http2_session& operator=(http2_session&&) noexcept;

    // Feed received bytes. Returns bytes consumed, or -1 on error.
    ssize_t feed(const uint8_t* data, size_t len);

    // Get bytes to send.
    std::string get_pending_output();

    bool is_alive() const;

    // Send pending output via the given send function
    Task<bool> flush(send_fn fn);

    // --- Server ---
    void set_request_handler(request_handler handler);
    void submit_response(int32_t stream_id, const response& resp);

    // Set push provider — called after each response to determine pushes
    void set_push_provider(push_provider provider);

    // Manually push a resource on a parent stream
    // Returns the promised stream ID, or -1 if push is disabled
    int32_t submit_push(int32_t parent_stream_id, const request& promised_req,
                        const response& push_resp);

    // --- Client ---
    void set_push_handler(push_handler handler);
    struct response_promise {
        response resp;
        bool complete = false;
        bool error = false;
        bool is_push = false;
        push_promise_info push_info;
        std::coroutine_handle<> waiter;
    };

    int32_t submit_request(const request& req, std::shared_ptr<response_promise> promise);
    std::vector<std::shared_ptr<response_promise>> take_completed_promises();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace async_net::http
