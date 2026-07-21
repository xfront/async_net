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

    // Streaming data callback: invoked for each DATA frame on a stream
    // Parameters: (stream_id, data, length, is_end_stream)
    using data_callback = std::function<void(int32_t, const uint8_t*, size_t, bool)>;

    // Streaming request handler: called when a request arrives.
    // Returns true if the handler takes ownership of the stream (streaming mode).
    // Returns false to fall through to the normal request_handler.
    // The handler receives (stream_id, request) and can set up data callbacks.
    using streaming_request_handler = std::function<bool(int32_t, const request&)>;

    // Request start handler: called when HEADERS are decoded.
    // Returns true if the handler takes ownership (streaming mode).
    // The bool parameter indicates whether END_STREAM was set on HEADERS (no body will follow).
    // This is called early so the handler can set up data callbacks before DATA frames arrive.
    using request_start_handler = std::function<bool(int32_t, const request&, bool end_stream)>;

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
    void set_streaming_request_handler(streaming_request_handler handler);
    void set_request_start_handler(request_start_handler handler);
    void submit_response(int32_t stream_id, const response& resp);

    // Streaming support: set data callback for a specific stream
    // When set, DATA frames are delivered to the callback instead of being buffered.
    // The callback receives (stream_id, data_ptr, data_len, end_stream).
    void set_data_callback(int32_t stream_id, data_callback cb);

    // Send response headers without body (for streaming responses)
    void submit_response_headers(int32_t stream_id, const response& resp);

    // Send a DATA frame on an open stream (for streaming)
    void submit_data(int32_t stream_id, const std::string& data, bool end_stream = false);

    // Send trailers (HEADERS frame with END_STREAM) on an open stream
    void submit_trailers(int32_t stream_id, const headers& trailers);

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
    // Submit request without END_STREAM on HEADERS (for streaming RPCs where body will be sent later)
    int32_t submit_request(const request& req, std::shared_ptr<response_promise> promise, bool no_end_stream);
    std::vector<std::shared_ptr<response_promise>> take_completed_promises();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace async_net::http
