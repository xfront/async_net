// gRPC Stream Deframer — accumulates raw bytes and extracts complete gRPC messages
// Shared by server and channel to avoid duplicating the 5-byte header parsing logic.
#pragma once

#include <async_net/grpc/types.hpp>
#include <async_net/grpc/stream.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace async_net::grpc {

// ============================================================================
// Stream Deframer
//
// Accumulates raw bytes from DATA frames and extracts complete gRPC messages
// (5-byte length-prefixed frames) into a reader.
//
// Usage:
//   auto deframer = std::make_shared<stream_deframer>(rdr);
//   // In data callback:
//   deframer->feed(data, len);
//   if (end_stream) deframer->complete();
// ============================================================================

class stream_deframer {
public:
    explicit stream_deframer(std::shared_ptr<reader> rdr)
        : rdr_(std::move(rdr))
    {
    }

    // Optional: set a callback invoked for each complete message extracted.
    // When set, messages are NOT pushed into the reader.
    void on_message(std::function<void(const std::string&)> cb) { on_msg_ = std::move(cb); }

    // Feed raw bytes from a DATA frame. Extracts complete gRPC messages
    // and pushes them into the reader (or invokes on_msg callback if set).
    void feed(const uint8_t* data, size_t len) {
        buf_.append(reinterpret_cast<const char*>(data), len);

        while (buf_.size() >= 5) {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buf_.data());
            uint32_t msg_len = (static_cast<uint32_t>(ptr[1]) << 24) |
                               (static_cast<uint32_t>(ptr[2]) << 16) |
                               (static_cast<uint32_t>(ptr[3]) << 8) |
                               static_cast<uint32_t>(ptr[4]);
            if (buf_.size() < 5 + msg_len) break;

            std::string frame_data(buf_.begin(), buf_.begin() + 5 + msg_len);
            buf_.erase(0, 5 + msg_len);

            auto proto = decode_grpc_message(frame_data);
            if (proto) {
                if (on_msg_) {
                    on_msg_(*proto);
                } else {
                    rdr_->push_message(*proto);
                }
            }
        }
    }

    // Convenience overload for (data, len, end_stream) callbacks
    void feed(const uint8_t* data, size_t len, bool end_stream) {
        feed(data, len);
        if (end_stream) complete();
    }

    // Signal that the stream has ended (no more messages will arrive)
    void complete() {
        rdr_->finish();
    }

    // Access the underlying reader
    std::shared_ptr<reader> get_reader() const { return rdr_; }

private:
    std::shared_ptr<reader> rdr_;
    std::string buf_;
    std::function<void(const std::string&)> on_msg_;
};

} // namespace async_net::grpc
