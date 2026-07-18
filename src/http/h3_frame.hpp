// HTTP/3 Frame Layer — RFC 9114
// Zero-dependency implementation
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

namespace async_net::http::h3 {

// ============================================================================
// QUIC Variable-Length Integer (RFC 9000 §16)
// ============================================================================

// Encode a QUIC varint. Returns bytes written (1, 2, 4, or 8), or 0 on error.
inline int encode_varint(uint8_t* buf, size_t bufsize, uint64_t value) {
    if (value < 64) {
        if (bufsize < 1) return 0;
        buf[0] = static_cast<uint8_t>(value);
        return 1;
    }
    if (value < 16384) {
        if (bufsize < 2) return 0;
        buf[0] = static_cast<uint8_t>((value >> 8) | 0x40);
        buf[1] = static_cast<uint8_t>(value & 0xFF);
        return 2;
    }
    if (value < 1073741824ULL) {
        if (bufsize < 4) return 0;
        buf[0] = static_cast<uint8_t>((value >> 24) | 0x80);
        buf[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buf[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buf[3] = static_cast<uint8_t>(value & 0xFF);
        return 4;
    }
    if (value < 4611686018427387904ULL) {
        if (bufsize < 8) return 0;
        buf[0] = static_cast<uint8_t>((value >> 56) | 0xC0);
        buf[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
        buf[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
        buf[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
        buf[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
        buf[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buf[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buf[7] = static_cast<uint8_t>(value & 0xFF);
        return 8;
    }
    return 0;
}

// Decode a QUIC varint. Returns bytes consumed, or 0 on error.
inline int decode_varint(const uint8_t* buf, size_t len, uint64_t& value) {
    if (len == 0) return 0;
    uint8_t prefix = buf[0] >> 6;
    switch (prefix) {
    case 0:
        value = buf[0] & 0x3F;
        return 1;
    case 1:
        if (len < 2) return 0;
        value = ((uint64_t(buf[0] & 0x3F) << 8) | buf[1]);
        return 2;
    case 2:
        if (len < 4) return 0;
        value = (uint64_t(buf[0] & 0x3F) << 24) |
                (uint64_t(buf[1]) << 16) |
                (uint64_t(buf[2]) << 8) |
                buf[3];
        return 4;
    case 3:
        if (len < 8) return 0;
        value = (uint64_t(buf[0] & 0x3F) << 56) |
                (uint64_t(buf[1]) << 48) |
                (uint64_t(buf[2]) << 40) |
                (uint64_t(buf[3]) << 32) |
                (uint64_t(buf[4]) << 24) |
                (uint64_t(buf[5]) << 16) |
                (uint64_t(buf[6]) << 8) |
                buf[7];
        return 8;
    }
    return 0;
}

// Append a varint to a string
inline void append_varint(std::string& out, uint64_t value) {
    uint8_t tmp[8];
    int n = encode_varint(tmp, sizeof(tmp), value);
    out.append(reinterpret_cast<const char*>(tmp), n);
}

// ============================================================================
// HTTP/3 Frame Types (RFC 9114 §7.2)
// ============================================================================

enum class frame_type : uint64_t {
    DATA           = 0x00,
    HEADERS        = 0x01,
    CANCEL_PUSH    = 0x03,
    SETTINGS       = 0x04,
    PUSH_PROMISE   = 0x05,
    GOAWAY         = 0x07,
    MAX_PUSH_ID    = 0x0D,
};

// ============================================================================
// HTTP/3 Settings Parameters (RFC 9114 §7.2.4.1)
// ============================================================================

enum class setting_id : uint64_t {
    MAX_FIELD_SECTION_SIZE = 0x06,
    // Grease values omitted
};

struct setting_entry {
    setting_id id;
    uint64_t value;
};

// ============================================================================
// HTTP/3 Unidirectional Stream Types (RFC 9114 §6.2)
// ============================================================================

enum class stream_type : uint64_t {
    CONTROL        = 0x00,
    PUSH           = 0x01,
    QPACK_ENCODER  = 0x02,
    QPACK_DECODER  = 0x03,
};

// ============================================================================
// Frame structure
// ============================================================================

struct frame {
    frame_type type;
    std::string payload;
};

// ============================================================================
// Frame builders
// ============================================================================

// Build a generic frame: type(varint) + length(varint) + payload
inline std::string build_frame(frame_type type, const std::string& payload) {
    std::string result;
    append_varint(result, static_cast<uint64_t>(type));
    append_varint(result, payload.size());
    result.append(payload);
    return result;
}

// Build DATA frame
inline std::string build_data_frame(const std::string& data) {
    return build_frame(frame_type::DATA, data);
}

// Build HEADERS frame (payload is QPACK-encoded field section)
inline std::string build_headers_frame(const std::string& qpack_data) {
    return build_frame(frame_type::HEADERS, qpack_data);
}

// Build SETTINGS frame
inline std::string build_settings_frame(const std::vector<setting_entry>& settings) {
    std::string payload;
    for (auto& s : settings) {
        append_varint(payload, static_cast<uint64_t>(s.id));
        append_varint(payload, s.value);
    }
    return build_frame(frame_type::SETTINGS, payload);
}

// Build GOAWAY frame
inline std::string build_goaway_frame(uint64_t stream_id) {
    std::string payload;
    append_varint(payload, stream_id);
    return build_frame(frame_type::GOAWAY, payload);
}

// Build CANCEL_PUSH frame
inline std::string build_cancel_push_frame(uint64_t push_id) {
    std::string payload;
    append_varint(payload, push_id);
    return build_frame(frame_type::CANCEL_PUSH, payload);
}

// Build PUSH_PROMISE frame
// Payload: Push ID (varint) + QPACK-encoded field section
inline std::string build_push_promise_frame(uint64_t push_id, const std::string& qpack_headers) {
    std::string payload;
    append_varint(payload, push_id);
    payload.append(qpack_headers);
    return build_frame(frame_type::PUSH_PROMISE, payload);
}

// Build MAX_PUSH_ID frame
inline std::string build_max_push_id_frame(uint64_t push_id) {
    std::string payload;
    append_varint(payload, push_id);
    return build_frame(frame_type::MAX_PUSH_ID, payload);
}

// Parse PUSH_PROMISE payload
struct push_promise_data {
    uint64_t push_id = 0;
    std::string header_block;
};

inline push_promise_data parse_push_promise(const uint8_t* data, size_t len) {
    push_promise_data pp;
    uint64_t pid;
    int n = decode_varint(data, len, pid);
    if (n <= 0) return pp;
    pp.push_id = pid;
    if (static_cast<size_t>(n) < len) {
        pp.header_block.assign(reinterpret_cast<const char*>(data + n), len - n);
    }
    return pp;
}

// ============================================================================
// Frame parser — incremental reader
// ============================================================================

class frame_reader {
public:
    enum class state {
        READ_TYPE,
        READ_LENGTH,
        READ_PAYLOAD,
        COMPLETE,
    };

    // Feed bytes. Returns bytes consumed.
    size_t feed(const uint8_t* data, size_t len) {
        size_t consumed = 0;
        while (consumed < len) {
            switch (state_) {
            case state::READ_TYPE: {
                // Accumulate varint bytes for type
                size_t n = feed_varint(data + consumed, len - consumed, type_val_);
                if (n == 0) return consumed;
                consumed += n;
                if (varint_complete_) {
                    current_type_ = static_cast<frame_type>(type_val_);
                    type_val_ = 0;
                    varint_complete_ = false;
                    state_ = state::READ_LENGTH;
                }
                break;
            }
            case state::READ_LENGTH: {
                size_t n = feed_varint(data + consumed, len - consumed, length_val_);
                if (n == 0) return consumed;
                consumed += n;
                if (varint_complete_) {
                    current_length_ = static_cast<size_t>(length_val_);
                    length_val_ = 0;
                    varint_complete_ = false;
                    payload_buf_.clear();
                    payload_buf_.reserve(current_length_);
                    state_ = state::READ_PAYLOAD;
                }
                break;
            }
            case state::READ_PAYLOAD: {
                size_t remaining = current_length_ - payload_buf_.size();
                size_t to_copy = std::min(remaining, len - consumed);
                payload_buf_.append(reinterpret_cast<const char*>(data + consumed), to_copy);
                consumed += to_copy;
                if (payload_buf_.size() == current_length_) {
                    state_ = state::COMPLETE;
                }
                break;
            }
            case state::COMPLETE:
                return consumed; // Must call take_frame() first
            }
        }
        return consumed;
    }

    bool has_frame() const { return state_ == state::COMPLETE; }

    frame take_frame() {
        state_ = state::READ_TYPE;
        type_val_ = 0;
        length_val_ = 0;
        varint_complete_ = false;
        return {current_type_, std::move(payload_buf_)};
    }

    void reset() {
        state_ = state::READ_TYPE;
        type_val_ = 0;
        length_val_ = 0;
        varint_complete_ = false;
        payload_buf_.clear();
    }

private:
    state state_ = state::READ_TYPE;
    frame_type current_type_ = frame_type::DATA;
    uint64_t type_val_ = 0;
    uint64_t length_val_ = 0;
    size_t current_length_ = 0;
    std::string payload_buf_;
    bool varint_complete_ = false;

    // Incremental varint decoder
    size_t feed_varint(const uint8_t* data, size_t len, uint64_t& accum) {
        if (len == 0) return 0;

        if (varint_bytes_expected_ == 0) {
            // First byte determines length
            uint8_t prefix = data[0] >> 6;
            switch (prefix) {
            case 0: varint_bytes_expected_ = 1; break;
            case 1: varint_bytes_expected_ = 2; break;
            case 2: varint_bytes_expected_ = 4; break;
            case 3: varint_bytes_expected_ = 8; break;
            }
            varint_buf_len_ = 0;
        }

        size_t needed = varint_bytes_expected_ - varint_buf_len_;
        size_t to_copy = std::min(needed, len);
        memcpy(varint_buf_ + varint_buf_len_, data, to_copy);
        varint_buf_len_ += to_copy;

        if (varint_buf_len_ == varint_bytes_expected_) {
            uint64_t val;
            int r = decode_varint(varint_buf_, varint_buf_len_, val);
            if (r > 0) {
                accum = val;
                varint_complete_ = true;
                varint_bytes_expected_ = 0;
                varint_buf_len_ = 0;
            }
        }
        return to_copy;
    }

    uint8_t varint_buf_[8] = {};
    size_t varint_buf_len_ = 0;
    size_t varint_bytes_expected_ = 0;
};

// ============================================================================
// Settings parser
// ============================================================================

inline std::vector<setting_entry> parse_settings(const uint8_t* data, size_t len) {
    std::vector<setting_entry> settings;
    size_t pos = 0;
    while (pos < len) {
        uint64_t id, value;
        int n = decode_varint(data + pos, len - pos, id);
        if (n <= 0) break;
        pos += n;
        n = decode_varint(data + pos, len - pos, value);
        if (n <= 0) break;
        pos += n;
        settings.push_back({static_cast<setting_id>(id), value});
    }
    return settings;
}

// ============================================================================
// GOAWAY parser
// ============================================================================

inline bool parse_goaway(const uint8_t* data, size_t len, uint64_t& stream_id) {
    int n = decode_varint(data, len, stream_id);
    return n > 0;
}

} // namespace async_net::http::h3
