// gRPC Channel implementation — client-side HTTP/2 transport
// Supports Unary, Server Streaming, Client Streaming, and Bidirectional Streaming RPCs.

#include <async_net/grpc/channel.hpp>
#include <async_net/grpc/stream_deframer.hpp>
#include <async_net/http/http2_session.hpp>
#include <iostream>
#include <cstring>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/net/ssl.hpp>
#endif

namespace async_net::grpc {

// ============================================================================
// Constructor
// ============================================================================

channel::channel(io_context& ctx, const std::string& host, uint16_t port)
    : ctx_(ctx)
    , host_(host)
    , port_(port)
    , sock_(ctx)
    , h2_(std::make_shared<h2_state>(host))
{
}

// ============================================================================
// Destructor
// ============================================================================

channel::~channel() {
    close();
}

// ============================================================================
// Connect (plain HTTP/2 with prior knowledge)
// ============================================================================

Task<bool> channel::connect() {
    int err = co_await sock_.async_connect(host_.c_str(), port_);
    if (err != 0) {
        std::cerr << "[grpc] Connect failed with error: " << err << std::endl;
        co_return false;
    }

    connected_ = true;

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };

    // Send HTTP/2 connection preface (24 bytes)
    static constexpr const char* H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    size_t preface_sent = 0;
    while (preface_sent < 24) {
        auto n = co_await sock_.async_write_some(
            const_buffer(H2_PREFACE + preface_sent, 24 - preface_sent));
        if (n <= 0) {
            std::cerr << "[grpc] Failed to send H2 preface" << std::endl;
            sock_.close();
            co_return false;
        }
        preface_sent += static_cast<size_t>(n);
    }

    // Send client SETTINGS frame
    co_await h2_->session.flush(send_fn);

    // Start background read loop
    read_task_ = new Task<void>(read_loop());
    read_task_->resume();

    co_return true;
}

#ifdef ASYNC_NET_HAS_SSL
Task<bool> channel::connect(ssl::context& ssl_ctx) {
    std::cerr << "[grpc] TLS connect not yet implemented" << std::endl;
    co_return false;
}
#endif

// ============================================================================
// Background read loop
// ============================================================================

Task<void> channel::read_loop() {
    char read_buf[16384];
    auto h2 = h2_;
    
    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };

    while (connected_ && h2->session.is_alive()) {
        auto n = co_await sock_.async_read_some(
            mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) {
            connected_ = false;
            break;
        }

        auto consumed = h2->session.feed(
            reinterpret_cast<const uint8_t*>(read_buf),
            static_cast<size_t>(n));
        if (consumed < 0) {
            connected_ = false;
            break;
        }

        bool ok = co_await h2->session.flush(send_fn);
        if (!ok) {
            connected_ = false;
            break;
        }
    }
}

// ============================================================================
// Helper: build HTTP/2 gRPC request
// ============================================================================

static http::request make_grpc_request(const std::string& host,
                                        const std::string& path,
                                        const std::string& body_data) {
    http::request req;
    req.method = http::method::POST;
    req.path = path;
    req.ver = http::version::HTTP_2;
    req.hdrs.set("Host", host);
    req.hdrs.set("Content-Type", grpc_constants::content_type);
    req.hdrs.set("TE", grpc_constants::te);
    req.hdrs.set("grpc-encoding", grpc_constants::grpc_encoding);
    if (!body_data.empty()) {
        req.bd = http::body(body_data);
    }
    return req;
}

// ============================================================================
// Helper: extract gRPC status from response
// ============================================================================

static status extract_grpc_status(const http::response& resp) {
    status grpc_status{status_code::ok, ""};
    
    auto status_hdr = resp.hdrs.get(grpc_constants::grpc_status);
    if (status_hdr) {
        int code = 0;
        std::from_chars(status_hdr->data(), status_hdr->data() + status_hdr->size(), code);
        grpc_status.code = static_cast<status_code>(code);
    }

    auto msg_hdr = resp.hdrs.get(grpc_constants::grpc_message);
    if (msg_hdr) {
        grpc_status.message = *msg_hdr;
    }

    return grpc_status;
}

// ============================================================================
// Unary RPC call
// ============================================================================

Task<std::pair<status, std::string>> channel::unary_call(
    const std::string& service,
    const std::string& method,
    const std::string& request_data)
{
    if (!connected_) {
        co_return std::make_pair(
            status{status_code::unavailable, "Channel not connected"},
            std::string{});
    }

    std::string path = make_path(service, method);
    auto req = make_grpc_request(host_, path, encode_grpc_message(request_data));

    auto promise = std::make_shared<http::http2_session::response_promise>();
    int32_t stream_id = h2_->session.submit_request(req, promise);
    if (stream_id < 0) {
        co_return std::make_pair(
            status{status_code::internal, "Failed to submit request"},
            std::string{});
    }

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };
    co_await h2_->session.flush(send_fn);

    co_await PromiseAwaiter{promise};

    if (promise->error) {
        co_return std::make_pair(
            status{status_code::internal, "Request error"},
            std::string{});
    }

    const auto& resp = promise->resp;
    if (resp.status.as_int() != 200) {
        co_return std::make_pair(
            status{status_code::internal, "HTTP error: " + std::to_string(resp.status.as_int())},
            std::string{});
    }

    status grpc_status = extract_grpc_status(resp);
    if (!grpc_status.ok()) {
        co_return std::make_pair(grpc_status, std::string{});
    }

    auto response_data = decode_grpc_message(resp.bd.data());
    if (!response_data) {
        co_return std::make_pair(
            status{status_code::internal, "Invalid gRPC response frame"},
            std::string{});
    }

    co_return std::make_pair(grpc_status, std::move(*response_data));
}

// ============================================================================
// Unary RPC call with metadata
// ============================================================================

Task<std::pair<status, std::string>> channel::unary_call(
    const std::string& service,
    const std::string& method,
    const std::string& request_data,
    const metadata& initial_metadata,
    metadata* response_metadata)
{
    if (!connected_) {
        co_return std::make_pair(
            status{status_code::unavailable, "Channel not connected"},
            std::string{});
    }

    std::string path = make_path(service, method);
    auto req = make_grpc_request(host_, path, encode_grpc_message(request_data));

    // Attach initial metadata as HTTP/2 headers
    for (auto& [k, v] : initial_metadata) {
        req.hdrs.set(k, v);
    }

    auto promise = std::make_shared<http::http2_session::response_promise>();
    int32_t stream_id = h2_->session.submit_request(req, promise);
    if (stream_id < 0) {
        co_return std::make_pair(
            status{status_code::internal, "Failed to submit request"},
            std::string{});
    }

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };
    co_await h2_->session.flush(send_fn);

    co_await PromiseAwaiter{promise};

    if (promise->error) {
        co_return std::make_pair(
            status{status_code::internal, "Request error"},
            std::string{});
    }

    const auto& resp = promise->resp;
    if (resp.status.as_int() != 200) {
        co_return std::make_pair(
            status{status_code::internal, "HTTP error: " + std::to_string(resp.status.as_int())},
            std::string{});
    }

    // Extract response metadata if requested
    if (response_metadata) {
        *response_metadata = call_context::extract_from_headers(resp.hdrs);
    }

    status grpc_status = extract_grpc_status(resp);
    if (!grpc_status.ok()) {
        co_return std::make_pair(grpc_status, std::string{});
    }

    auto response_data = decode_grpc_message(resp.bd.data());
    if (!response_data) {
        co_return std::make_pair(
            status{status_code::internal, "Invalid gRPC response frame"},
            std::string{});
    }

    co_return std::make_pair(grpc_status, std::move(*response_data));
}

// ============================================================================
// Server-streaming RPC call
// ============================================================================

Task<status> channel::server_stream_call(
    const std::string& service,
    const std::string& method,
    const std::string& request_data,
    std::function<void(const std::string&)> on_message)
{
    if (!connected_) {
        co_return status{status_code::unavailable, "Channel not connected"};
    }

    std::string path = make_path(service, method);
    auto req = make_grpc_request(host_, path, encode_grpc_message(request_data));

    auto promise = std::make_shared<http::http2_session::response_promise>();
    int32_t stream_id = h2_->session.submit_request(req, promise);
    if (stream_id < 0) {
        co_return status{status_code::internal, "Failed to submit request"};
    }

    // Set up data callback to receive streaming messages
    auto deframer = std::make_shared<stream_deframer>(nullptr);
    if (on_message) {
        deframer->on_message(on_message);
    }

    h2_->session.set_data_callback(stream_id,
        [deframer](int32_t, const uint8_t* data, size_t len, bool) {
            deframer->feed(data, len);
        });

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };
    co_await h2_->session.flush(send_fn);

    // Wait for stream to complete (trailers)
    co_await PromiseAwaiter{promise};

    if (promise->error) {
        co_return status{status_code::internal, "Request error"};
    }

    co_return extract_grpc_status(promise->resp);
}

// ============================================================================
// Client-streaming RPC call
// ============================================================================

Task<channel::client_stream_state> channel::client_stream_call(
    const std::string& service,
    const std::string& method)
{
    if (!connected_) {
        co_return client_stream_state{nullptr, nullptr};
    }

    std::string path = make_path(service, method);
    // Submit request without body (body will be sent as DATA frames)
    auto req = make_grpc_request(host_, path, "");

    auto promise = std::make_shared<http::http2_session::response_promise>();
    // Use no_end_stream=true so HEADERS doesn't have END_STREAM (data will follow)
    int32_t stream_id = h2_->session.submit_request(req, promise, true);
    if (stream_id < 0) {
        co_return client_stream_state{nullptr, nullptr};
    }

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };

    auto wrt = std::make_shared<writer>(&h2_->session, stream_id);

    // Flush the headers
    co_await h2_->session.flush(send_fn);

    co_return client_stream_state{wrt, promise};
}

// ============================================================================
// Bidirectional streaming RPC call
// ============================================================================

Task<channel::bidi_stream_state> channel::bidi_stream_call(
    const std::string& service,
    const std::string& method)
{
    if (!connected_) {
        co_return bidi_stream_state{nullptr, nullptr};
    }

    std::string path = make_path(service, method);
    auto req = make_grpc_request(host_, path, "");

    auto promise = std::make_shared<http::http2_session::response_promise>();
    // Use no_end_stream=true so HEADERS doesn't have END_STREAM (data will follow)
    int32_t stream_id = h2_->session.submit_request(req, promise, true);
    if (stream_id < 0) {
        co_return bidi_stream_state{nullptr, nullptr};
    }

    auto send_fn = [this](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock_.async_write_some(const_buffer(data, len));
    };

    // Set up reader for incoming messages
    auto rdr = std::make_shared<reader>();
    auto deframer = std::make_shared<stream_deframer>(rdr);

    h2_->session.set_data_callback(stream_id,
        [deframer](int32_t, const uint8_t* data, size_t len, bool end_stream) {
            deframer->feed(data, len, end_stream);
        });

    auto wrt = std::make_shared<writer>(&h2_->session, stream_id);

    co_await h2_->session.flush(send_fn);

    co_return bidi_stream_state{rdr, wrt};
}

// ============================================================================
// Close the channel
// ============================================================================

void channel::close() {
    connected_ = false;
    sock_.close();
    read_task_ = nullptr;
}

} // namespace async_net::grpc
