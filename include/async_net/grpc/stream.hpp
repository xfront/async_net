// gRPC Streaming — reader/writer abstractions for streaming RPCs
// reader: receives gRPC messages from a stream (client-streaming / bidi input)
// writer: sends gRPC messages to a stream (server-streaming / bidi output)
#pragma once

#include <async_net/grpc/types.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/http/http2_session.hpp>
#include <deque>
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <coroutine>

namespace async_net::grpc {

// ============================================================================
// Message Reader — receives gRPC messages from a stream
// ============================================================================

class reader {
public:
    reader() = default;

    // Read the next gRPC message. Returns nullopt when the stream ends.
    Task<std::optional<std::string>> read();

    // Check if the stream has ended
    bool is_done() const { return done_; }

    // --- Internal API (used by server/channel) ---

    // Push raw protobuf data into the reader (called by data callback)
    void push_message(std::string data);

    // Signal that the stream has ended
    void finish();

private:
    std::deque<std::string> messages_;
    bool done_ = false;
    std::coroutine_handle<> waiter_;
};

// ============================================================================
// Message Writer — sends gRPC messages to a stream
// ============================================================================

class writer {
public:
    writer() = default;
    writer(http::http2_session* session, int32_t stream_id);

    // Send a gRPC message (protobuf data will be framed)
    Task<bool> write(const std::string& message);

    // Finish the stream with a gRPC status (sends trailers)
    Task<void> finish(status_code code = status_code::ok, const std::string& msg = "");

    // Check if the writer has been finished
    bool is_finished() const { return finished_; }

private:
    http::http2_session* session_ = nullptr;
    int32_t stream_id_ = 0;
    bool finished_ = false;
};

} // namespace async_net::grpc
