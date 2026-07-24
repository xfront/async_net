// HTTP/2 Frame Layer — zero-dependency implementation
// RFC 7540: https://tools.ietf.org/html/rfc7540
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <async_net/detail/config.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <arpa/inet.h>  // htonl, ntohl
#endif

namespace async_net::http::h2 {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t FRAME_HEADER_SIZE = 9;
static constexpr size_t SETTINGS_PARAM_SIZE = 6;
static constexpr size_t DEFAULT_MAX_FRAME_SIZE = 16384;
static constexpr size_t DEFAULT_WINDOW_SIZE = 65535;
static constexpr uint32_t MAX_WINDOW_SIZE = 0x7FFFFFFF;

// Connection preface (client sends this first)
static constexpr const char* CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
static constexpr size_t CONNECTION_PREFACE_SIZE = 24;

// ---------------------------------------------------------------------------
// Frame Types (RFC 7540 §6)
// ---------------------------------------------------------------------------

enum class frame_type : uint8_t {
    DATA          = 0x0,
    HEADERS       = 0x1,
    PRIORITY      = 0x2,
    RST_STREAM    = 0x3,
    SETTINGS      = 0x4,
    PUSH_PROMISE  = 0x5,
    PING          = 0x6,
    GOAWAY        = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION  = 0x9,
};

inline const char* frame_type_name(frame_type t) {
    switch (t) {
        case frame_type::DATA:          return "DATA";
        case frame_type::HEADERS:       return "HEADERS";
        case frame_type::PRIORITY:      return "PRIORITY";
        case frame_type::RST_STREAM:    return "RST_STREAM";
        case frame_type::SETTINGS:      return "SETTINGS";
        case frame_type::PUSH_PROMISE:  return "PUSH_PROMISE";
        case frame_type::PING:          return "PING";
        case frame_type::GOAWAY:        return "GOAWAY";
        case frame_type::WINDOW_UPDATE: return "WINDOW_UPDATE";
        case frame_type::CONTINUATION:  return "CONTINUATION";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Frame Flags
// ---------------------------------------------------------------------------

namespace flag {
    static constexpr uint8_t NONE          = 0x00;
    static constexpr uint8_t ACK           = 0x01;  // SETTINGS, PING
    static constexpr uint8_t END_STREAM    = 0x01;  // DATA, HEADERS
    static constexpr uint8_t END_HEADERS   = 0x04;  // HEADERS, CONTINUATION, PUSH_PROMISE
    static constexpr uint8_t PADDED        = 0x08;  // DATA, HEADERS, PUSH_PROMISE
    static constexpr uint8_t PRIORITY      = 0x20;  // HEADERS
}

// ---------------------------------------------------------------------------
// Error Codes (RFC 7540 §7)
// ---------------------------------------------------------------------------

enum class error_code : uint32_t {
    NO_ERROR            = 0x0,
    PROTOCOL_ERROR      = 0x1,
    INTERNAL_ERROR      = 0x2,
    FLOW_CONTROL_ERROR  = 0x3,
    SETTINGS_TIMEOUT    = 0x4,
    STREAM_CLOSED       = 0x5,
    FRAME_SIZE_ERROR    = 0x6,
    REFUSED_STREAM      = 0x7,
    CANCEL              = 0x8,
    COMPRESSION_ERROR   = 0x9,
    CONNECT_ERROR       = 0xA,
    ENHANCE_YOUR_CALM   = 0xB,
    INADEQUATE_SECURITY = 0xC,
    HTTP_1_1_REQUIRED   = 0xD,
};

// ---------------------------------------------------------------------------
// Settings Parameters (RFC 7540 §6.5.2)
// ---------------------------------------------------------------------------

enum class setting_id : uint16_t {
    HEADER_TABLE_SIZE      = 0x1,
    ENABLE_PUSH            = 0x2,
    MAX_CONCURRENT_STREAMS = 0x3,
    INITIAL_WINDOW_SIZE    = 0x4,
    MAX_FRAME_SIZE         = 0x5,
    MAX_HEADER_LIST_SIZE   = 0x6,
};

struct setting_entry {
    setting_id id;
    uint32_t value;
};

// ---------------------------------------------------------------------------
// Frame Header — 9 bytes on the wire
// ---------------------------------------------------------------------------

struct frame_header {
    uint32_t  length    = 0;      // 24-bit payload length
    frame_type type     = frame_type::DATA;
    uint8_t   flags     = 0;
    int32_t   stream_id = 0;      // 31-bit (bit 0 is reserved, always 0)

    bool has_flag(uint8_t f) const { return (flags & f) != 0; }

    // Serialize to 9-byte wire format (big-endian)
    void write_to(uint8_t* buf) const {
        // Length: 3 bytes big-endian
        buf[0] = static_cast<uint8_t>((length >> 16) & 0xFF);
        buf[1] = static_cast<uint8_t>((length >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>(length & 0xFF);
        buf[3] = static_cast<uint8_t>(type);
        buf[4] = flags;
        // Stream ID: 4 bytes big-endian, bit 0 reserved
        uint32_t sid = static_cast<uint32_t>(stream_id) & 0x7FFFFFFF;
        buf[5] = static_cast<uint8_t>((sid >> 24) & 0xFF);
        buf[6] = static_cast<uint8_t>((sid >> 16) & 0xFF);
        buf[7] = static_cast<uint8_t>((sid >> 8) & 0xFF);
        buf[8] = static_cast<uint8_t>(sid & 0xFF);
    }

    // Parse from 9-byte buffer
    static frame_header read_from(const uint8_t* buf) {
        frame_header fh;
        fh.length = (uint32_t(buf[0]) << 16) | (uint32_t(buf[1]) << 8) | uint32_t(buf[2]);
        fh.type = static_cast<frame_type>(buf[3]);
        fh.flags = buf[4];
        fh.stream_id = static_cast<int32_t>(
            (uint32_t(buf[5]) << 24) | (uint32_t(buf[6]) << 16) |
            (uint32_t(buf[7]) << 8)  | uint32_t(buf[8])) & 0x7FFFFFFF;
        return fh;
    }
};

// ---------------------------------------------------------------------------
// Frame — header + payload
// ---------------------------------------------------------------------------

struct frame {
    frame_header hdr;
    std::string  payload;
};

// ---------------------------------------------------------------------------
// Frame Builder helpers
// ---------------------------------------------------------------------------

// Build a complete frame (header + payload) as a string
inline std::string build_frame(frame_type type, uint8_t flags,
                                int32_t stream_id, const std::string& payload) {
    std::string result;
    result.resize(FRAME_HEADER_SIZE + payload.size());
    frame_header fh;
    fh.length = static_cast<uint32_t>(payload.size());
    fh.type = type;
    fh.flags = flags;
    fh.stream_id = stream_id;
    fh.write_to(reinterpret_cast<uint8_t*>(result.data()));
    if (!payload.empty()) {
        std::memcpy(result.data() + FRAME_HEADER_SIZE, payload.data(), payload.size());
    }
    return result;
}

// Build a SETTINGS frame with given parameters
inline std::string build_settings_frame(const std::vector<setting_entry>& settings,
                                         bool ack = false) {
    std::string payload;
    if (!ack) {
        payload.resize(settings.size() * SETTINGS_PARAM_SIZE);
        for (size_t i = 0; i < settings.size(); ++i) {
            uint8_t* p = reinterpret_cast<uint8_t*>(payload.data() + i * SETTINGS_PARAM_SIZE);
            uint16_t id = static_cast<uint16_t>(settings[i].id);
            uint32_t val = settings[i].value;
            p[0] = static_cast<uint8_t>((id >> 8) & 0xFF);
            p[1] = static_cast<uint8_t>(id & 0xFF);
            p[2] = static_cast<uint8_t>((val >> 24) & 0xFF);
            p[3] = static_cast<uint8_t>((val >> 16) & 0xFF);
            p[4] = static_cast<uint8_t>((val >> 8) & 0xFF);
            p[5] = static_cast<uint8_t>(val & 0xFF);
        }
    }
    return build_frame(frame_type::SETTINGS, ack ? flag::ACK : flag::NONE, 0, payload);
}

// Build a WINDOW_UPDATE frame
inline std::string build_window_update(int32_t stream_id, uint32_t increment) {
    std::string payload(4, '\0');
    uint8_t* p = reinterpret_cast<uint8_t*>(payload.data());
    uint32_t inc = increment & 0x7FFFFFFF;
    p[0] = static_cast<uint8_t>((inc >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((inc >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((inc >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(inc & 0xFF);
    return build_frame(frame_type::WINDOW_UPDATE, 0, stream_id, payload);
}

// Build a GOAWAY frame
inline std::string build_goaway(int32_t last_stream_id, error_code ec,
                                 const std::string& debug_data = "") {
    std::string payload(8 + debug_data.size(), '\0');
    uint8_t* p = reinterpret_cast<uint8_t*>(payload.data());
    uint32_t sid = static_cast<uint32_t>(last_stream_id) & 0x7FFFFFFF;
    p[0] = static_cast<uint8_t>((sid >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((sid >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((sid >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(sid & 0xFF);
    uint32_t err = static_cast<uint32_t>(ec);
    p[4] = static_cast<uint8_t>((err >> 24) & 0xFF);
    p[5] = static_cast<uint8_t>((err >> 16) & 0xFF);
    p[6] = static_cast<uint8_t>((err >> 8) & 0xFF);
    p[7] = static_cast<uint8_t>(err & 0xFF);
    if (!debug_data.empty()) {
        std::memcpy(p + 8, debug_data.data(), debug_data.size());
    }
    return build_frame(frame_type::GOAWAY, 0, 0, payload);
}

// Build a RST_STREAM frame
inline std::string build_rst_stream(int32_t stream_id, error_code ec) {
    std::string payload(4, '\0');
    uint8_t* p = reinterpret_cast<uint8_t*>(payload.data());
    uint32_t err = static_cast<uint32_t>(ec);
    p[0] = static_cast<uint8_t>((err >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((err >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((err >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(err & 0xFF);
    return build_frame(frame_type::RST_STREAM, 0, stream_id, payload);
}

// Build a PING frame
inline std::string build_ping(const uint8_t opaque_data[8], bool ack = false) {
    std::string payload(reinterpret_cast<const char*>(opaque_data), 8);
    return build_frame(frame_type::PING, ack ? flag::ACK : 0, 0, payload);
}

// Parse SETTINGS parameters from payload
inline std::vector<setting_entry> parse_settings(const std::string& payload) {
    std::vector<setting_entry> result;
    if (payload.size() % SETTINGS_PARAM_SIZE != 0) return result;
    for (size_t i = 0; i < payload.size(); i += SETTINGS_PARAM_SIZE) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data() + i);
        setting_entry se;
        se.id = static_cast<setting_id>((uint16_t(p[0]) << 8) | p[1]);
        se.value = (uint32_t(p[2]) << 24) | (uint32_t(p[3]) << 16) |
                   (uint32_t(p[4]) << 8) | uint32_t(p[5]);
        result.push_back(se);
    }
    return result;
}

// Parse WINDOW_UPDATE increment from payload
inline uint32_t parse_window_update(const std::string& payload) {
    if (payload.size() < 4) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    return ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3])) & 0x7FFFFFFF;
}

// Build a PUSH_PROMISE frame
// Payload: 4-byte Promised Stream ID + HPACK-encoded header block
inline std::string build_push_promise(int32_t stream_id, int32_t promised_stream_id,
                                       const std::string& header_block) {
    std::string payload;
    payload.resize(4 + header_block.size());
    uint8_t* p = reinterpret_cast<uint8_t*>(payload.data());
    uint32_t psid = static_cast<uint32_t>(promised_stream_id) & 0x7FFFFFFF;
    p[0] = static_cast<uint8_t>((psid >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((psid >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((psid >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(psid & 0xFF);
    if (!header_block.empty()) {
        std::memcpy(p + 4, header_block.data(), header_block.size());
    }
    // Always set END_HEADERS (we don't use CONTINUATION for push promises)
    return build_frame(frame_type::PUSH_PROMISE, flag::END_HEADERS, stream_id, payload);
}

// Parse PUSH_PROMISE payload
struct push_promise_data {
    int32_t promised_stream_id = 0;
    std::string header_block;
};

inline push_promise_data parse_push_promise(const std::string& payload) {
    push_promise_data pp;
    if (payload.size() < 4) return pp;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    pp.promised_stream_id = static_cast<int32_t>(
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8)  | uint32_t(p[3])) & 0x7FFFFFFF;
    if (payload.size() > 4) {
        pp.header_block = payload.substr(4);
    }
    return pp;
}

// Parse GOAWAY from payload
struct goaway_data {
    int32_t last_stream_id = 0;
    error_code ec = error_code::NO_ERROR;
    std::string debug_data;
};

inline goaway_data parse_goaway(const std::string& payload) {
    goaway_data g;
    if (payload.size() < 8) return g;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    g.last_stream_id = static_cast<int32_t>(
        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | uint32_t(p[3])) & 0x7FFFFFFF;
    g.ec = static_cast<error_code>(
        (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) |
        (uint32_t(p[6]) << 8) | uint32_t(p[7]));
    if (payload.size() > 8) {
        g.debug_data = payload.substr(8);
    }
    return g;
}

// ---------------------------------------------------------------------------
// Frame Reader — buffered incremental frame parser
// ---------------------------------------------------------------------------

class frame_reader {
public:
    // Append raw bytes. Returns number of bytes consumed.
    size_t feed(const uint8_t* data, size_t len) {
        size_t consumed = 0;
        while (consumed < len) {
            if (header_buf_len_ < FRAME_HEADER_SIZE) {
                // Still reading frame header
                size_t need = FRAME_HEADER_SIZE - header_buf_len_;
                size_t take = std::min(need, len - consumed);
                std::memcpy(header_buf_ + header_buf_len_, data + consumed, take);
                header_buf_len_ += take;
                consumed += take;
            } else if (payload_buf_.size() < current_length_) {
                // Reading payload
                size_t need = current_length_ - payload_buf_.size();
                size_t take = std::min(need, len - consumed);
                payload_buf_.append(reinterpret_cast<const char*>(data + consumed), take);
                consumed += take;
            } else {
                // Should not happen — caller should call take_frame() first
                break;
            }

            // Check if header is complete and we know the payload length
            if (header_buf_len_ == FRAME_HEADER_SIZE && current_length_ == SIZE_MAX) {
                current_header_ = frame_header::read_from(header_buf_);
                current_length_ = current_header_.length;
                payload_buf_.clear();
                payload_buf_.reserve(current_length_);
            }
        }
        return consumed;
    }

    // Check if a complete frame is available
    bool has_frame() const {
        return header_buf_len_ == FRAME_HEADER_SIZE &&
               payload_buf_.size() >= current_length_;
    }

    // Take the completed frame. Resets internal state for next frame.
    frame take_frame() {
        frame f;
        f.hdr = current_header_;
        f.payload = std::move(payload_buf_);
        // Reset state
        header_buf_len_ = 0;
        current_length_ = SIZE_MAX;
        payload_buf_.clear();
        return f;
    }

private:
    uint8_t header_buf_[FRAME_HEADER_SIZE] = {};
    size_t header_buf_len_ = 0;
    frame_header current_header_;
    size_t current_length_ = SIZE_MAX;  // SIZE_MAX means header not yet parsed
    std::string payload_buf_;
};

} // namespace async_net::http::h2
