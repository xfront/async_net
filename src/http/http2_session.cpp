// HTTP/2 Session implementation — zero-dependency (no nghttp2)
#include <async_net/http/http2_session.hpp>
#include "h2_frame.hpp"
#include "hpack.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <map>

namespace async_net::http {

// Helper: convert header name to lowercase (HTTP/2 requires lowercase headers)
static std::string to_lower_header(const std::string& hdr) {
    std::string lower = hdr;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

// ============================================================================
// Internal implementation (pimpl)
// ============================================================================

struct http2_session::impl {
    using mode = http2_session::mode;

    // Per-stream state
    struct stream_state {
        int32_t stream_id = 0;
        enum st { idle, open, half_closed_local, half_closed_remote, closed };
        st state = idle;

        request req;
        response resp;
        std::string body;
        bool headers_complete = false;
        bool end_stream = false;
        int64_t send_window = static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE);
        int64_t recv_window = static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE);
        std::shared_ptr<http2_session::response_promise> promise;

        // Streaming: data callback (when set, DATA frames go to callback instead of body buffer)
        http2_session::data_callback data_cb;

        // Set when request_start_handler has taken ownership of this stream
        bool streaming_handled = false;

        // For CONTINUATION frames
        std::string pending_header_block;
    };

    mode mode_;
    bool alive_ = true;
    bool preface_received_ = false;
    bool flushing_ = false;  // Prevents concurrent flush calls

    h2::frame_reader reader_;
    std::string output_buf_;
    h2::hpack_encoder encoder_;
    h2::hpack_decoder decoder_;
    std::map<int32_t, stream_state> streams_;
    int32_t next_stream_id_ = 1;
    int32_t last_client_stream_ = 0;
    int64_t conn_send_window_ = static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE);
    int64_t conn_recv_window_ = static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE);
    size_t peer_max_frame_size_ = h2::DEFAULT_MAX_FRAME_SIZE;
    uint32_t peer_initial_window_size_ = h2::DEFAULT_WINDOW_SIZE;
    request_handler request_handler_;
    http2_session::streaming_request_handler streaming_request_handler_;
    http2_session::request_start_handler request_start_handler_;
    http2_session::push_provider push_provider_;
    http2_session::push_handler push_handler_;
    std::vector<std::shared_ptr<http2_session::response_promise>> completed_promises_;

    // For CONTINUATION: which stream is expecting continuation frames
    int32_t continuation_stream_ = 0;

    // Push support
    bool peer_enable_push_ = true;  // Client supports push?
    int32_t next_push_stream_id_ = 2;  // Server push uses even stream IDs

    explicit impl(mode m) : mode_(m) {
        if (mode_ == mode::server) {
            next_stream_id_ = 2;  // Server uses even IDs for push (odd for client)
        }
        // Send initial SETTINGS
        std::vector<h2::setting_entry> settings = {
            {h2::setting_id::MAX_CONCURRENT_STREAMS, 100},
            {h2::setting_id::INITIAL_WINDOW_SIZE, 65535},
            {h2::setting_id::MAX_HEADER_LIST_SIZE, 8192},
            {h2::setting_id::ENABLE_PUSH, 1},
        };
        enqueue(h2::build_settings_frame(settings));
    }

    void enqueue(const std::string& data) {
        output_buf_.append(data);
    }

    // --- Feed bytes ---
    ssize_t feed(const uint8_t* data, size_t len) {
        if (!alive_) return -1;

        // Server: check connection preface
        if (mode_ == mode::server && !preface_received_) {
            preface_received_ = true;
        }

        // Feed all bytes, draining complete frames as they become available
        size_t total_consumed = 0;
        while (total_consumed < len && alive_) {
            size_t consumed = reader_.feed(data + total_consumed, len - total_consumed);
            total_consumed += consumed;

            // Process all complete frames
            while (reader_.has_frame()) {
                auto f = reader_.take_frame();
                process_frame(f);
                if (!alive_) break;
            }

            // If no bytes were consumed and no frames were processed, stop
            if (consumed == 0 && !reader_.has_frame()) break;
        }
        return static_cast<ssize_t>(total_consumed);
    }

    // --- Process a single frame ---
    void process_frame(h2::frame& f) {
        // If we're expecting CONTINUATION frames, only allow CONTINUATION
        if (continuation_stream_ != 0) {
            if (f.hdr.type != h2::frame_type::CONTINUATION ||
                f.hdr.stream_id != continuation_stream_) {
                // Protocol error
                goaway(h2::error_code::PROTOCOL_ERROR, "Expected CONTINUATION");
                return;
            }
            handle_continuation(f);
            return;
        }

        switch (f.hdr.type) {
            case h2::frame_type::DATA:          handle_data(f); break;
            case h2::frame_type::HEADERS:        handle_headers(f); break;
            case h2::frame_type::PRIORITY:       /* ignore */ break;
            case h2::frame_type::RST_STREAM:     handle_rst_stream(f); break;
            case h2::frame_type::SETTINGS:       handle_settings(f); break;
            case h2::frame_type::PUSH_PROMISE:   handle_push_promise(f); break;
            case h2::frame_type::PING:           handle_ping(f); break;
            case h2::frame_type::GOAWAY:         handle_goaway(f); break;
            case h2::frame_type::WINDOW_UPDATE:  handle_window_update(f); break;
            case h2::frame_type::CONTINUATION:
                goaway(h2::error_code::PROTOCOL_ERROR, "Unexpected CONTINUATION");
                break;
        }
    }

    // --- DATA frame ---
    void handle_data(h2::frame& f) {
        int32_t sid = f.hdr.stream_id;
        if (sid == 0) { goaway(h2::error_code::PROTOCOL_ERROR, "DATA on stream 0"); return; }

        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            // Stream might have been closed, send RST_STREAM
            enqueue(h2::build_rst_stream(sid, h2::error_code::STREAM_CLOSED));
            return;
        }

        auto& ss = it->second;
        uint8_t* payload = reinterpret_cast<uint8_t*>(f.payload.data());
        size_t payload_len = f.payload.size();

        // Handle padding
        size_t offset = 0;
        if (f.hdr.has_flag(h2::flag::PADDED)) {
            if (payload_len < 1) { goaway(h2::error_code::FRAME_SIZE_ERROR); return; }
            uint8_t pad_len = payload[0];
            offset = 1;
            if (offset + pad_len > payload_len) { goaway(h2::error_code::PROTOCOL_ERROR); return; }
            payload_len -= (1 + pad_len);
        }

        // Accumulate body (or deliver to streaming callback)
        if (ss.data_cb) {
            bool end_stream_flag = f.hdr.has_flag(h2::flag::END_STREAM);
            ss.data_cb(sid, payload + offset, payload_len, end_stream_flag);
        } else {
            ss.body.append(reinterpret_cast<const char*>(payload + offset), payload_len);
        }

        // Update flow control (connection + stream)
        conn_recv_window_ -= static_cast<int64_t>(payload_len);
        ss.recv_window -= static_cast<int64_t>(payload_len);
        // Window updates sent lazily when needed
        if (conn_recv_window_ < static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE) / 2) {
            uint32_t increment = static_cast<uint32_t>(static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE) - conn_recv_window_);
            enqueue(h2::build_window_update(0, increment));
            conn_recv_window_ += increment;
        }
        if (ss.recv_window < static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE) / 2) {
            uint32_t increment = static_cast<uint32_t>(static_cast<int64_t>(h2::DEFAULT_WINDOW_SIZE) - ss.recv_window);
            enqueue(h2::build_window_update(sid, increment));
            ss.recv_window += increment;
        }

        // Check END_STREAM
        if (f.hdr.has_flag(h2::flag::END_STREAM)) {
            ss.end_stream = true;
            if (mode_ == mode::server) {
                // Don't dispatch if streaming handler already took ownership
                if (!ss.streaming_handled) {
                    dispatch_request(sid, ss);
                }
            } else {
                complete_promise(ss);
            }
        }
    }

    // --- HEADERS frame ---
    void handle_headers(h2::frame& f) {
        int32_t sid = f.hdr.stream_id;
        if (sid == 0) { goaway(h2::error_code::PROTOCOL_ERROR, "HEADERS on stream 0"); return; }

        uint8_t* payload = reinterpret_cast<uint8_t*>(f.payload.data());
        size_t payload_len = f.payload.size();
        size_t offset = 0;

        // Handle padding
        if (f.hdr.has_flag(h2::flag::PADDED)) {
            if (payload_len < 1) { goaway(h2::error_code::FRAME_SIZE_ERROR); return; }
            uint8_t pad_len = payload[0];
            offset = 1;
            if (offset + pad_len > payload_len) { goaway(h2::error_code::PROTOCOL_ERROR); return; }
            payload_len -= (1 + pad_len);
        }

        // Handle priority
        if (f.hdr.has_flag(h2::flag::PRIORITY)) {
            if (payload_len < 5) { goaway(h2::error_code::FRAME_SIZE_ERROR); return; }
            offset += 5;
            payload_len -= 5;
        }

        // Extract header block fragment
        const uint8_t* header_block = payload + offset;
        size_t header_block_len = payload_len;

        bool end_headers = f.hdr.has_flag(h2::flag::END_HEADERS);
        bool end_stream = f.hdr.has_flag(h2::flag::END_STREAM);

        if (mode_ == mode::server) {
            // Check if this is a trailer HEADERS on an already-established streaming stream
            auto existing_it = streams_.find(sid);
            if (existing_it != streams_.end() && existing_it->second.streaming_handled && end_stream) {
                // This is trailers (second HEADERS with END_STREAM) on a streaming stream.
                // Signal end_stream to the data_callback so it can finalize.
                auto& ss = existing_it->second;
                if (ss.data_cb) {
                    ss.data_cb(sid, nullptr, 0, true);
                }
                ss.end_stream = true;
                return;
            }

            // Create or find stream
            auto& ss = streams_[sid];
            ss.stream_id = sid;
            ss.state = stream_state::open;
            last_client_stream_ = std::max(last_client_stream_, sid);

            if (end_headers) {
                // Decode HPACK immediately
                auto headers = decoder_.decode(header_block, header_block_len);
                apply_headers(sid, ss, headers, end_stream);
            } else {
                // Need CONTINUATION frames
                ss.pending_header_block.assign(
                    reinterpret_cast<const char*>(header_block), header_block_len);
                continuation_stream_ = sid;
                if (end_stream) ss.end_stream = true;  // Remember for later
            }
        } else {
            // Client: response headers
            auto it = streams_.find(sid);
            if (it == streams_.end()) return;
            auto& ss = it->second;

            // Check if this is trailer HEADERS on a streaming response
            // (stream already has headers_complete and a data_cb registered)
            if (ss.headers_complete && ss.data_cb && end_stream) {
                // Signal end_stream to the data_callback
                ss.data_cb(sid, nullptr, 0, true);
                // Still apply trailers and complete promise (for PromiseAwaiter)
                auto headers = decoder_.decode(header_block, header_block_len);
                apply_response_headers(sid, ss, headers, end_stream);
                return;
            }

            if (end_headers) {
                auto headers = decoder_.decode(header_block, header_block_len);
                apply_response_headers(sid, ss, headers, end_stream);
            } else {
                ss.pending_header_block.assign(
                    reinterpret_cast<const char*>(header_block), header_block_len);
                continuation_stream_ = sid;
                if (end_stream) ss.end_stream = true;
            }
        }
    }

    void apply_headers(int32_t sid, stream_state& ss,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       bool end_stream) {
        ss.headers_complete = true;
        for (auto& [name, value] : headers) {
            if (name == ":method") {
                auto m = parse_method(value);
                if (m) ss.req.method = *m;
            } else if (name == ":path") {
                // Split path and query string
                auto qpos = value.find('?');
                if (qpos != std::string::npos) {
                    ss.req.path = value.substr(0, qpos);
                    ss.req.query = value.substr(qpos + 1);
                } else {
                    ss.req.path = value;
                }
                if (ss.req.path.empty()) ss.req.path = "/";
            } else if (name == ":scheme" || name == ":authority") {
                if (name == ":authority") ss.req.hdrs.append("Host", value);
            } else if (name[0] != ':') {
                ss.req.hdrs.append(name, value);
            }
        }
        ss.req.ver = version::HTTP_2;

        if (end_stream) {
            ss.end_stream = true;
            if (request_start_handler_) {
                // Let the start handler decide
                if (request_start_handler_(sid, ss.req, true)) {
                    ss.streaming_handled = true;
                    // Streaming handler took ownership — no dispatch needed
                } else {
                    // Not taken by streaming handler, dispatch normally
                    dispatch_request(sid, ss);
                }
            } else {
                dispatch_request(sid, ss);
            }
        } else if (request_start_handler_) {
            // Headers complete but body not yet received (streaming request)
            // Notify the handler so it can set up data callbacks
            if (request_start_handler_(sid, ss.req, false)) {
                ss.streaming_handled = true;
            }
        }
    }

    void apply_response_headers(int32_t sid, stream_state& ss,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                bool end_stream) {
        ss.headers_complete = true;
        for (auto& [name, value] : headers) {
            if (name == ":status") {
                int code = 0;
                std::from_chars(value.data(), value.data() + value.size(), code);
                ss.resp.status = status_code(code);
            } else if (name[0] != ':') {
                ss.resp.hdrs.append(name, value);
            }
        }
        ss.resp.ver = version::HTTP_2;

        if (end_stream) {
            ss.end_stream = true;
            complete_promise(ss);
        }
    }

    // --- CONTINUATION frame ---
    void handle_continuation(h2::frame& f) {
        auto it = streams_.find(f.hdr.stream_id);
        if (it == streams_.end()) {
            goaway(h2::error_code::PROTOCOL_ERROR, "CONTINUATION for unknown stream");
            return;
        }

        auto& ss = it->second;
        ss.pending_header_block.append(f.payload);

        if (f.hdr.has_flag(h2::flag::END_HEADERS)) {
            continuation_stream_ = 0;
            auto headers = decoder_.decode(
                reinterpret_cast<const uint8_t*>(ss.pending_header_block.data()),
                ss.pending_header_block.size());
            ss.pending_header_block.clear();

            bool end_stream = ss.end_stream;  // Was set from HEADERS frame
            if (mode_ == mode::server) {
                apply_headers(f.hdr.stream_id, ss, headers, end_stream);
            } else {
                apply_response_headers(f.hdr.stream_id, ss, headers, end_stream);
            }
        }
    }

    // --- SETTINGS frame ---
    void handle_settings(h2::frame& f) {
        if (f.hdr.stream_id != 0) {
            goaway(h2::error_code::PROTOCOL_ERROR, "SETTINGS on non-zero stream");
            return;
        }

        if (f.hdr.has_flag(h2::flag::ACK)) {
            // ACK received for our settings
            return;
        }

        if (f.payload.size() % h2::SETTINGS_PARAM_SIZE != 0) {
            goaway(h2::error_code::FRAME_SIZE_ERROR);
            return;
        }

        auto settings = h2::parse_settings(f.payload);
        for (auto& s : settings) {
            switch (s.id) {
                case h2::setting_id::HEADER_TABLE_SIZE:
                    encoder_.set_max_table_size(s.value);
                    break;
                case h2::setting_id::MAX_CONCURRENT_STREAMS:
                    // We don't enforce this strictly
                    break;
                case h2::setting_id::INITIAL_WINDOW_SIZE:
                    if (s.value > h2::MAX_WINDOW_SIZE) {
                        goaway(h2::error_code::FLOW_CONTROL_ERROR);
                        return;
                    }
                    // Update existing streams' send windows
                    {
                        int64_t delta = static_cast<int64_t>(s.value) -
                                       static_cast<int64_t>(peer_initial_window_size_);
                        for (auto& [sid, ss] : streams_) {
                            ss.send_window += delta;
                        }
                    }
                    peer_initial_window_size_ = s.value;
                    break;
                case h2::setting_id::MAX_FRAME_SIZE:
                    if (s.value < 16384 || s.value > 16777215) {
                        goaway(h2::error_code::PROTOCOL_ERROR, "Invalid MAX_FRAME_SIZE");
                        return;
                    }
                    peer_max_frame_size_ = s.value;
                    break;
                case h2::setting_id::MAX_HEADER_LIST_SIZE:
                    // Advisory, we don't enforce
                    break;
                case h2::setting_id::ENABLE_PUSH:
                    peer_enable_push_ = (s.value != 0);
                    break;
            }
        }

        // Send SETTINGS ACK
        enqueue(h2::build_settings_frame({}, true));
    }

    // --- PING frame ---
    void handle_ping(h2::frame& f) {
        if (f.hdr.stream_id != 0) {
            goaway(h2::error_code::PROTOCOL_ERROR, "PING on non-zero stream");
            return;
        }
        if (f.payload.size() != 8) {
            goaway(h2::error_code::FRAME_SIZE_ERROR);
            return;
        }
        if (!f.hdr.has_flag(h2::flag::ACK)) {
            // Reply with PING ACK
            enqueue(h2::build_ping(reinterpret_cast<const uint8_t*>(f.payload.data()), true));
        }
    }

    // --- WINDOW_UPDATE frame ---
    void handle_window_update(h2::frame& f) {
        if (f.payload.size() != 4) {
            goaway(h2::error_code::FRAME_SIZE_ERROR);
            return;
        }
        uint32_t increment = h2::parse_window_update(f.payload);
        if (increment == 0) {
            if (f.hdr.stream_id == 0)
                goaway(h2::error_code::PROTOCOL_ERROR, "WINDOW_UPDATE 0 increment");
            else
                enqueue(h2::build_rst_stream(f.hdr.stream_id, h2::error_code::PROTOCOL_ERROR));
            return;
        }

        if (f.hdr.stream_id == 0) {
            conn_send_window_ += increment;
            if (conn_send_window_ > static_cast<int64_t>(h2::MAX_WINDOW_SIZE)) {
                goaway(h2::error_code::FLOW_CONTROL_ERROR);
            }
        } else {
            auto it = streams_.find(f.hdr.stream_id);
            if (it != streams_.end()) {
                it->second.send_window += increment;
                if (it->second.send_window > static_cast<int64_t>(h2::MAX_WINDOW_SIZE)) {
                    enqueue(h2::build_rst_stream(f.hdr.stream_id,
                                                  h2::error_code::FLOW_CONTROL_ERROR));
                }
            }
        }
    }

    // --- GOAWAY frame ---
    void handle_goaway(h2::frame& f) {
        auto g = h2::parse_goaway(f.payload);
        std::cerr << "[h2] GOAWAY received: last_stream=" << g.last_stream_id
                  << " error=" << static_cast<int>(g.ec);
        if (!g.debug_data.empty()) std::cerr << " debug=" << g.debug_data;
        std::cerr << std::endl;
        alive_ = false;
    }

    // --- RST_STREAM frame ---
    void handle_rst_stream(h2::frame& f) {
        if (f.payload.size() != 4) {
            goaway(h2::error_code::FRAME_SIZE_ERROR);
            return;
        }
        auto it = streams_.find(f.hdr.stream_id);
        if (it != streams_.end()) {
            if (it->second.promise) {
                it->second.promise->error = true;
                it->second.promise->complete = true;
                completed_promises_.push_back(it->second.promise);
                if (it->second.promise->waiter) it->second.promise->waiter.resume();
            }
            streams_.erase(it);
        }
    }

    // --- PUSH_PROMISE (client-side handling) ---
    void handle_push_promise(h2::frame& f) {
        if (mode_ == mode::server) {
            // Servers must not receive PUSH_PROMISE
            goaway(h2::error_code::PROTOCOL_ERROR, "PUSH_PROMISE on server");
            return;
        }
        auto pp = h2::parse_push_promise(f.payload);
        int32_t parent_sid = f.hdr.stream_id;

        // Decode the promised request headers
        auto headers = decoder_.decode(
            reinterpret_cast<const uint8_t*>(pp.header_block.data()),
            pp.header_block.size());

        http2_session::push_promise_info info;
        info.promised_stream_id = pp.promised_stream_id;
        for (auto& [name, value] : headers) {
            if (name == ":method") {
                auto m = parse_method(value);
                if (m) info.promised_request.method = *m;
            } else if (name == ":path") {
                info.promised_request.path = value;
            } else if (name == ":scheme" || name == ":authority") {
                if (name == ":authority") info.promised_request.hdrs.append("Host", value);
            } else if (name[0] != ':') {
                info.promised_request.hdrs.append(name, value);
            }
        }
        info.promised_request.ver = version::HTTP_2;

        // Create a stream_state for the promised stream
        auto& ss = streams_[pp.promised_stream_id];
        ss.stream_id = pp.promised_stream_id;
        ss.state = stream_state::open;

        // Create a response promise for the push
        ss.promise = std::make_shared<http2_session::response_promise>();
        ss.promise->is_push = true;
        ss.promise->push_info = info;

        // Notify via push handler
        if (push_handler_) {
            push_handler_(info);
        }
    }

    // --- Dispatch completed request (server) ---
    void dispatch_request(int32_t sid, stream_state& ss) {
        ss.req.bd = body(std::move(ss.body));
        ss.body.clear();

        // Try streaming handler first
        if (streaming_request_handler_ && streaming_request_handler_(sid, ss.req)) {
            return; // Streaming handler took ownership
        }

        if (request_handler_) {
            response resp = request_handler_(ss.req);
            do_submit_response(sid, resp);

            // Invoke push provider to send associated resources
            if (push_provider_ && peer_enable_push_) {
                auto pushes = push_provider_(ss.req);
                for (auto& [promised_req, push_resp] : pushes) {
                    do_submit_push(sid, promised_req, push_resp);
                }
            }
        } else {
            do_submit_response(sid, response_not_found());
        }
    }

    // --- Complete response promise (client) ---
    void complete_promise(stream_state& ss) {
        ss.resp.bd = body(std::move(ss.body));
        ss.body.clear();
        if (ss.promise) {
            ss.promise->resp = std::move(ss.resp);
            ss.promise->complete = true;
            completed_promises_.push_back(ss.promise);
            if (ss.promise->waiter) ss.promise->waiter.resume();
        }
    }

    // --- Submit response (server) ---
    void do_submit_response(int32_t stream_id, const response& resp) {
        // Build response headers
        std::string status_str = std::to_string(resp.status.as_int());
        std::vector<std::pair<std::string, std::string>> hdrs;
        hdrs.emplace_back(":status", status_str);

        auto ct = resp.hdrs.get("Content-Type");
        if (ct) hdrs.emplace_back("content-type", *ct);

        if (!resp.bd.empty()) {
            hdrs.emplace_back("content-length", std::to_string(resp.bd.size()));
        }

        // Additional headers
        for (auto& [k, v] : resp.hdrs) {
            std::string lk = to_lower_header(k);
            if (lk == "content-type" || lk == "content-length" || lk == "connection" ||
                lk == "transfer-encoding") continue;
            hdrs.emplace_back(lk, v);
        }

        bool has_body = !resp.bd.empty();
        bool has_trailers = !resp.trailers.empty();
        
        // If we have trailers, don't set END_STREAM on headers or data
        uint8_t flags = (has_body || has_trailers) ? 0 : h2::flag::END_STREAM;

        enqueue(build_headers_frame(stream_id, flags, hdrs));

        if (has_body) {
            // If trailers follow, don't set END_STREAM on data frames
            enqueue(build_data_frames(stream_id, resp.bd.data(), has_trailers));
        }

        // Send trailers if present (as HEADERS frame with END_STREAM)
        if (has_trailers) {
            std::vector<std::pair<std::string, std::string>> trailer_hdrs;
            for (auto& [k, v] : resp.trailers) {
                std::string lk = to_lower_header(k);
                trailer_hdrs.emplace_back(lk, v);
            }
            enqueue(build_headers_frame(stream_id, h2::flag::END_STREAM, trailer_hdrs));
        }

        // Mark stream as closed (server side)
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            it->second.state = stream_state::closed;
        }
    }

    // --- Submit response headers only (for streaming responses) ---
    void do_submit_response_headers(int32_t stream_id, const response& resp) {
        std::string status_str = std::to_string(resp.status.as_int());
        std::vector<std::pair<std::string, std::string>> hdrs;
        hdrs.emplace_back(":status", status_str);

        auto ct = resp.hdrs.get("Content-Type");
        if (ct) hdrs.emplace_back("content-type", *ct);

        // Additional headers
        for (auto& [k, v] : resp.hdrs) {
            std::string lk = to_lower_header(k);
            if (lk == "content-type" || lk == "content-length" || lk == "connection" ||
                lk == "transfer-encoding") continue;
            hdrs.emplace_back(lk, v);
        }

        // No END_STREAM — stream stays open for streaming data
        enqueue(build_headers_frame(stream_id, 0, hdrs));
    }

    // --- Submit DATA frame on an open stream ---
    void do_submit_data(int32_t stream_id, const std::string& data, bool end_stream) {
        uint8_t flags = end_stream ? h2::flag::END_STREAM : 0;
        if (data.empty()) {
            enqueue(h2::build_frame(h2::frame_type::DATA, flags, stream_id, ""));
        } else {
            // Split into chunks if needed
            size_t offset = 0;
            while (offset < data.size()) {
                size_t chunk_size = std::min(peer_max_frame_size_, data.size() - offset);
                std::string chunk = data.substr(offset, chunk_size);
                offset += chunk_size;
                bool is_last = (offset >= data.size());
                uint8_t f = (end_stream && is_last) ? h2::flag::END_STREAM : 0;
                enqueue(h2::build_frame(h2::frame_type::DATA, f, stream_id, chunk));
            }
        }

        if (end_stream) {
            auto it = streams_.find(stream_id);
            if (it != streams_.end()) {
                it->second.state = stream_state::closed;
            }
        }
    }

    // --- Submit trailers (HEADERS + END_STREAM) ---
    void do_submit_trailers(int32_t stream_id, const headers& trailers) {
        std::vector<std::pair<std::string, std::string>> trailer_hdrs;
        for (auto& [k, v] : trailers) {
            std::string lk = to_lower_header(k);
            trailer_hdrs.emplace_back(lk, v);
        }
        enqueue(build_headers_frame(stream_id, h2::flag::END_STREAM, trailer_hdrs));

        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            it->second.state = stream_state::closed;
        }
    }

    // --- Build HEADERS frame ---
    std::string build_headers_frame(int32_t stream_id, uint8_t flags,
                                     const std::vector<std::pair<std::string, std::string>>& hdrs) {
        std::string block = encoder_.encode(hdrs);

        // Split into HEADERS + CONTINUATION if needed
        if (block.size() <= peer_max_frame_size_) {
            flags |= h2::flag::END_HEADERS;
            return h2::build_frame(h2::frame_type::HEADERS, flags, stream_id, block);
        }

        // First chunk in HEADERS
        std::string result;
        std::string first_chunk = block.substr(0, peer_max_frame_size_);
        result.append(h2::build_frame(h2::frame_type::HEADERS, flags, stream_id, first_chunk));

        // Remaining chunks in CONTINUATION
        size_t offset = peer_max_frame_size_;
        while (offset < block.size()) {
            size_t chunk_size = std::min(peer_max_frame_size_, block.size() - offset);
            std::string chunk = block.substr(offset, chunk_size);
            offset += chunk_size;
            uint8_t cont_flags = (offset >= block.size()) ? h2::flag::END_HEADERS : 0;
            result.append(h2::build_frame(h2::frame_type::CONTINUATION, cont_flags,
                                          stream_id, chunk));
        }
        return result;
    }

    // --- Build DATA frames ---
    std::string build_data_frames(int32_t stream_id, const std::string& body_data, bool no_end_stream = false) {
        std::string result;
        size_t offset = 0;

        while (offset < body_data.size()) {
            size_t chunk_size = std::min(peer_max_frame_size_, body_data.size() - offset);
            std::string chunk = body_data.substr(offset, chunk_size);
            offset += chunk_size;

            // Set END_STREAM on last chunk only if no trailers follow
            uint8_t flags = (!no_end_stream && offset >= body_data.size()) ? h2::flag::END_STREAM : 0;
            result.append(h2::build_frame(h2::frame_type::DATA, flags, stream_id, chunk));
        }

        // Empty body with END_STREAM (shouldn't happen since we check has_body)
        if (body_data.empty() && !no_end_stream) {
            result.append(h2::build_frame(h2::frame_type::DATA, h2::flag::END_STREAM,
                                          stream_id, ""));
        }
        return result;
    }

    // --- Submit request (client) ---
    int32_t do_submit_request(const request& req,
                              std::shared_ptr<http2_session::response_promise> promise,
                              bool no_end_stream = false) {
        int32_t stream_id = next_stream_id_;
        next_stream_id_ += 2;

        auto& ss = streams_[stream_id];
        ss.stream_id = stream_id;
        ss.state = stream_state::open;
        ss.promise = promise;

        // Build request headers
        std::vector<std::pair<std::string, std::string>> hdrs;
        hdrs.emplace_back(":method", to_string(req.method));
        hdrs.emplace_back(":path", req.path.empty() ? "/" : req.path);
        hdrs.emplace_back(":scheme", "https");
        auto host = req.hdrs.get("Host").value_or("localhost");
        hdrs.emplace_back(":authority", host);

        for (auto& [k, v] : req.hdrs) {
            std::string lk = to_lower_header(k);
            if (lk == "host" || lk == "connection") continue;
            hdrs.emplace_back(lk, v);
        }

        bool has_body = !req.bd.empty();
        // For streaming: don't set END_STREAM on HEADERS even without body
        uint8_t flags = (has_body || no_end_stream) ? 0 : h2::flag::END_STREAM;
        enqueue(build_headers_frame(stream_id, flags, hdrs));

        if (has_body) {
            enqueue(build_data_frames(stream_id, req.bd.data()));
        }

        return stream_id;
    }

    // --- Submit push (server, internal) ---
    int32_t do_submit_push(int32_t parent_stream_id, const request& promised_req,
                           const response& push_resp) {
        if (!peer_enable_push_) return -1;

        int32_t promised_sid = next_push_stream_id_;
        next_push_stream_id_ += 2;

        // Build the promised request headers for PUSH_PROMISE
        std::vector<std::pair<std::string, std::string>> push_hdrs;
        push_hdrs.emplace_back(":method", to_string(promised_req.method));
        push_hdrs.emplace_back(":path", promised_req.path.empty() ? "/" : promised_req.path);
        push_hdrs.emplace_back(":scheme", "https");
        auto host = promised_req.hdrs.get("Host").value_or("localhost");
        push_hdrs.emplace_back(":authority", host);
        for (auto& [k, v] : promised_req.hdrs) {
            std::string lk = to_lower_header(k);
            if (lk == "host" || lk == "connection") continue;
            push_hdrs.emplace_back(lk, v);
        }

        // Send PUSH_PROMISE on the parent stream
        std::string header_block = encoder_.encode(push_hdrs);
        enqueue(h2::build_push_promise(parent_stream_id, promised_sid, header_block));

        // Create stream state for the promised stream
        auto& ss = streams_[promised_sid];
        ss.stream_id = promised_sid;
        ss.state = stream_state::half_closed_local;

        // Send HEADERS + DATA on the promised stream
        do_submit_response(promised_sid, push_resp);

        return promised_sid;
    }

    // --- GOAWAY ---
    void goaway(h2::error_code ec, const std::string& debug = "") {
        enqueue(h2::build_goaway(last_client_stream_, ec, debug));
        alive_ = false;
    }

    // --- Output ---
    std::string get_pending_output() {
        std::string result = std::move(output_buf_);
        output_buf_.clear();
        return result;
    }
};

// ============================================================================
// Public interface — delegates to impl
// ============================================================================

http2_session::http2_session(mode m) : impl_(std::make_unique<impl>(m)) {}
http2_session::~http2_session() = default;
http2_session::http2_session(http2_session&&) noexcept = default;
http2_session& http2_session::operator=(http2_session&&) noexcept = default;

ssize_t http2_session::feed(const uint8_t* data, size_t len) {
    return impl_->feed(data, len);
}

std::string http2_session::get_pending_output() {
    return impl_->get_pending_output();
}

bool http2_session::is_alive() const {
    return impl_->alive_;
}

Task<bool> http2_session::flush(send_fn fn) {
    // Prevent concurrent flush calls (e.g., from data_callback inside another flush)
    if (impl_->flushing_) {
        // Another flush is in progress — it will pick up our data after finishing
        co_return true;
    }
    impl_->flushing_ = true;

    bool all_ok = true;
    while (all_ok) {
        auto data = get_pending_output();
        if (data.empty()) break;

        size_t sent = 0;
        while (sent < data.size()) {
            auto n = co_await fn(reinterpret_cast<const uint8_t*>(data.data() + sent),
                                  data.size() - sent);
            if (n <= 0) {
                all_ok = false;
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }

    impl_->flushing_ = false;
    co_return all_ok;
}

void http2_session::set_request_handler(request_handler handler) {
    impl_->request_handler_ = std::move(handler);
}

void http2_session::set_streaming_request_handler(streaming_request_handler handler) {
    impl_->streaming_request_handler_ = std::move(handler);
}

void http2_session::set_request_start_handler(request_start_handler handler) {
    impl_->request_start_handler_ = std::move(handler);
}

void http2_session::submit_response(int32_t stream_id, const response& resp) {
    impl_->do_submit_response(stream_id, resp);
}

void http2_session::set_data_callback(int32_t stream_id, data_callback cb) {
    auto it = impl_->streams_.find(stream_id);
    if (it != impl_->streams_.end()) {
        it->second.data_cb = std::move(cb);
    }
}

void http2_session::submit_response_headers(int32_t stream_id, const response& resp) {
    impl_->do_submit_response_headers(stream_id, resp);
}

void http2_session::submit_data(int32_t stream_id, const std::string& data, bool end_stream) {
    impl_->do_submit_data(stream_id, data, end_stream);
}

void http2_session::submit_trailers(int32_t stream_id, const headers& trailers) {
    impl_->do_submit_trailers(stream_id, trailers);
}

int32_t http2_session::submit_request(const request& req,
                                       std::shared_ptr<response_promise> promise) {
    return impl_->do_submit_request(req, std::move(promise));
}

int32_t http2_session::submit_request(const request& req,
                                       std::shared_ptr<response_promise> promise,
                                       bool no_end_stream) {
    return impl_->do_submit_request(req, std::move(promise), no_end_stream);
}

std::vector<std::shared_ptr<http2_session::response_promise>>
http2_session::take_completed_promises() {
    auto result = std::move(impl_->completed_promises_);
    impl_->completed_promises_.clear();
    return result;
}

void http2_session::set_push_provider(push_provider provider) {
    impl_->push_provider_ = std::move(provider);
}

int32_t http2_session::submit_push(int32_t parent_stream_id, const request& promised_req,
                                    const response& push_resp) {
    return impl_->do_submit_push(parent_stream_id, promised_req, push_resp);
}

void http2_session::set_push_handler(push_handler handler) {
    impl_->push_handler_ = std::move(handler);
}

} // namespace async_net::http

