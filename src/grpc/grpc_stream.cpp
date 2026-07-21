// gRPC Streaming implementation — reader/writer

#include <async_net/grpc/stream.hpp>
#include <iostream>

namespace async_net::grpc {

// ============================================================================
// Reader implementation
// ============================================================================

Task<std::optional<std::string>> reader::read() {
    // If there are buffered messages, return immediately
    if (!messages_.empty()) {
        std::string msg = std::move(messages_.front());
        messages_.pop_front();
        co_return msg;
    }

    // If stream has ended, return nullopt
    if (done_) {
        co_return std::nullopt;
    }

    // Suspend until a message arrives or stream ends
    struct ReadAwaiter {
        reader* r;

        bool await_ready() const noexcept {
            return !r->messages_.empty() || r->done_;
        }

        void await_suspend(std::coroutine_handle<> h) {
            r->waiter_ = h;
        }

        std::optional<std::string> await_resume() {
            if (!r->messages_.empty()) {
                std::string msg = std::move(r->messages_.front());
                r->messages_.pop_front();
                return msg;
            }
            return std::nullopt; // stream ended
        }
    };

    co_return co_await ReadAwaiter{this};
}

void reader::push_message(std::string data) {
    messages_.push_back(std::move(data));
    // Resume waiter if one is waiting
    if (waiter_) {
        auto h = waiter_;
        waiter_ = nullptr;
        h.resume();
    }
}

void reader::finish() {
    done_ = true;
    // Resume waiter if one is waiting (it will get nullopt)
    if (waiter_) {
        auto h = waiter_;
        waiter_ = nullptr;
        h.resume();
    }
}

// ============================================================================
// Writer implementation
// ============================================================================

writer::writer(http::http2_session* session, int32_t stream_id)
    : session_(session)
    , stream_id_(stream_id)
{
}

Task<bool> writer::write(const std::string& message) {
    if (finished_ || !session_) {
        co_return false;
    }

    // Encode as gRPC frame
    std::string frame = encode_grpc_message(message);

    // Submit DATA frame (don't end stream)
    // Don't flush here — the outer read loop's flush will send it.
    // Flushing from within a data_callback causes concurrent flush issues.
    session_->submit_data(stream_id_, frame, false);
    co_return true;
}

Task<void> writer::finish(status_code code, const std::string& msg) {
    if (finished_ || !session_) {
        co_return;
    }
    finished_ = true;

    // Send trailers with gRPC status
    http::headers trailers;
    trailers.set(grpc_constants::grpc_status, std::to_string(static_cast<int>(code)));
    if (!msg.empty()) {
        trailers.set(grpc_constants::grpc_message, msg);
    }
    session_->submit_trailers(stream_id_, trailers);
    // Don't flush here — the outer read loop's flush will send it.
}

} // namespace async_net::grpc
