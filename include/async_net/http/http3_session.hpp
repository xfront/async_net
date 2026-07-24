// HTTP/3 Session — wolfSSL + ngtcp2 + self-contained QPACK/H3 framing
#pragma once

#include <async_net/detail/config.hpp>
#include <async_net/http/types.hpp>
#include <async_net/http/handler.hpp>
#include <async_net/coroutine/task.hpp>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace async_net::http {

class http3_session {
public:
    using send_fn = std::function<Task<ssize_t>(const uint8_t*, size_t)>;
    using request_handler = std::function<response(const request&)>;

    // Push promise info — delivered to client when PUSH_PROMISE is received
    struct push_promise_info {
        uint64_t push_id = 0;
        request promised_request;
    };

    // Server: callback to determine which resources to push for a given request
    using push_provider = std::function<std::vector<std::pair<request, response>>(const request&)>;

    // Client: callback when PUSH_PROMISE is received
    using push_handler = std::function<void(const push_promise_info&)>;

    enum class mode { server, client };

    // QUIC connection configuration
    struct config {
        std::string cert_file;
        std::string key_file;
        uint64_t max_streams = 100;
        uint64_t idle_timeout_ms = 30000;
        std::string host;       // Server hostname (client only)
        uint16_t port = 443;    // Server port (client only)
    };

    // Internal implementation (forward-declared for callback access)
    struct impl;

    http3_session(mode m, const config& cfg);
    explicit http3_session(mode m);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) noexcept;
    http3_session& operator=(http3_session&&) noexcept;

    // Feed a received UDP packet (from the QUIC transport layer)
    // local_addr/remote_addr provide the path info for ngtcp2
    ssize_t feed_packet(const uint8_t* data, size_t len,
                         const ::sockaddr* local_addr = nullptr, ::socklen_t local_addrlen = 0,
                         const ::sockaddr* remote_addr = nullptr, ::socklen_t remote_addrlen = 0);

    // Server: initialize from first received packet (extracts DCID/SCID)
    // Returns true if initialization succeeded
    bool init_server_from_packet(const uint8_t* data, size_t len,
                                  const ::sockaddr* local_addr, ::socklen_t local_addrlen,
                                  const ::sockaddr* remote_addr, ::socklen_t remote_addrlen);

    // Get packets to send (returns QUIC packets to transmit via UDP)
    std::string get_pending_output();

    // Get individual QUIC packets (each should be sent as a separate UDP datagram)
    std::vector<std::string> get_pending_packets();

    bool is_alive() const;
    bool handshake_complete() const;

    // Send pending QUIC packets
    Task<bool> flush(send_fn fn);

    // Handle connection timeout
    void handle_expiry();

    // Get timeout expiry in milliseconds (or -1 if none)
    int64_t get_expiry() const;

    // --- Server ---
    void set_request_handler(request_handler handler);

    // Set push provider — called after each response to determine pushes
    void set_push_provider(push_provider provider);

    // Manually push a resource. Returns push_id, or -1 on failure.
    int64_t submit_push(const request& promised_req, const response& push_resp,
                        int64_t associated_stream_id);

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

    // Get the internal QUIC connection handle (for advanced use)
    void* native_handle() const;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace async_net::http
