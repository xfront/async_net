#pragma once

// WebSocket protocol implementation (RFC 6455)
// Provides: frame codec, handshake, and websocket_connection for async I/O.

#include <async_net/io/tcp.hpp>
#include <async_net/io/buffer.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/http/types.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <functional>
#include <algorithm>
#include <random>

namespace async_net::http::ws {

// ---------------------------------------------------------------------------
// Opcodes (RFC 6455 Section 5.2)
// ---------------------------------------------------------------------------

enum class opcode : uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA
};

// ---------------------------------------------------------------------------
// WebSocket Frame
// ---------------------------------------------------------------------------

struct frame {
    bool fin = true;
    opcode op = opcode::text;
    bool masked = false;
    std::string payload;
};

// ---------------------------------------------------------------------------
// SHA-1 (lightweight, for handshake key computation only)
// ---------------------------------------------------------------------------

namespace sha1_detail {

inline uint32_t left_rotate(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

inline std::string compute(const std::string& input) {
    // Initialize hash values
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    // Pre-processing: add padding
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t original_len = msg.size() * 8;

    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }

    // Append original length in bits (big-endian)
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>(original_len >> (i * 8)));
    }

    // Process each 512-bit chunk
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[offset + i * 4]) << 24) |
                    (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 16) |
                    (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 8) |
                    static_cast<uint32_t>(msg[offset + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d; d = c;
            c = left_rotate(b, 30);
            b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    // Produce the final hash value (big-endian), 20 bytes
    std::string result(20, '\0');
    auto store32 = [&](int offset, uint32_t val) {
        result[offset]   = static_cast<char>((val >> 24) & 0xFF);
        result[offset+1] = static_cast<char>((val >> 16) & 0xFF);
        result[offset+2] = static_cast<char>((val >> 8) & 0xFF);
        result[offset+3] = static_cast<char>(val & 0xFF);
    };
    store32(0, h0); store32(4, h1); store32(8, h2);
    store32(12, h3); store32(16, h4);

    return result;
}

}  // namespace sha1_detail

// ---------------------------------------------------------------------------
// Base64 encoder
// ---------------------------------------------------------------------------

namespace base64_detail {

inline std::string encode(const std::string& input) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    size_t len = input.size();
    output.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t a = static_cast<uint8_t>(input[i]);
        uint32_t b = (i + 1 < len) ? static_cast<uint8_t>(input[i + 1]) : 0;
        uint32_t c = (i + 2 < len) ? static_cast<uint8_t>(input[i + 2]) : 0;

        uint32_t triple = (a << 16) | (b << 8) | c;

        output += table[(triple >> 18) & 0x3F];
        output += table[(triple >> 12) & 0x3F];
        output += (i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=';
        output += (i + 2 < len) ? table[triple & 0x3F] : '=';
    }

    return output;
}

}  // namespace base64_detail

// ---------------------------------------------------------------------------
// WebSocket handshake helpers
// ---------------------------------------------------------------------------

// Magic GUID from RFC 6455
inline constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key
inline std::string compute_accept_key(const std::string& key) {
    std::string concat = key + kWsGuid;
    std::string sha1 = sha1_detail::compute(concat);
    return base64_detail::encode(sha1);
}

// Check if an HTTP request is a WebSocket upgrade request
inline bool is_websocket_upgrade(const request& req) {
    if (req.method != method::GET) return false;

    auto upgrade = req.hdrs.get("Upgrade");
    if (!upgrade) return false;

    std::string lower;
    lower.resize(upgrade->size());
    std::transform(upgrade->begin(), upgrade->end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (lower != "websocket") return false;

    auto connection = req.hdrs.get("Connection");
    if (!connection) return false;

    // Connection header may contain multiple values
    std::string conn_lower;
    conn_lower.resize(connection->size());
    std::transform(connection->begin(), connection->end(), conn_lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (conn_lower.find("upgrade") == std::string::npos) return false;

    return req.hdrs.contains("Sec-WebSocket-Key");
}

// Build the 101 Switching Protocols response
inline std::string build_handshake_response(const request& req) {
    auto key = req.hdrs.get("Sec-WebSocket-Key");
    if (!key) return "";

    std::string accept = compute_accept_key(*key);

    std::string resp;
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: " + accept + "\r\n";
    resp += "\r\n";
    return resp;
}

// ---------------------------------------------------------------------------
// Frame encoding (server -> client, no masking per RFC 6455)
// ---------------------------------------------------------------------------

inline std::string encode_frame(opcode op, const std::string& payload, bool fin = true) {
    std::string frame;
    frame.reserve(2 + (payload.size() > 125 ? 2 : 0) + payload.size());

    // First byte: FIN + opcode
    uint8_t byte0 = static_cast<uint8_t>(op);
    if (fin) byte0 |= 0x80;
    frame += static_cast<char>(byte0);

    // Second byte: MASK=0 + payload length
    if (payload.size() <= 125) {
        frame += static_cast<char>(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((payload.size() >> 8) & 0xFF);
        frame += static_cast<char>(payload.size() & 0xFF);
    } else {
        frame += static_cast<char>(127);
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame += static_cast<char>((len >> (i * 8)) & 0xFF);
        }
    }

    // Payload (no masking for server->client)
    frame += payload;
    return frame;
}

// ---------------------------------------------------------------------------
// Frame parsing (client -> server, with unmasking)
// ---------------------------------------------------------------------------

struct parse_result {
    frame frm;
    size_t consumed;
};

// Try to parse a WebSocket frame from raw data.
// Returns nullopt if more data is needed.
inline std::optional<parse_result> parse_frame(const uint8_t* data, size_t len) {
    if (len < 2) return std::nullopt;

    size_t pos = 0;
    uint8_t byte0 = data[pos++];
    uint8_t byte1 = data[pos++];

    bool fin = (byte0 & 0x80) != 0;
    auto op = static_cast<opcode>(byte0 & 0x0F);
    bool masked = (byte1 & 0x80) != 0;
    uint64_t payload_len = byte1 & 0x7F;

    if (payload_len == 126) {
        if (len < 4) return std::nullopt;
        payload_len = (static_cast<uint64_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        if (len < 10) return std::nullopt;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[pos + i];
        }
        pos += 8;
    }

    // Masking key (4 bytes if masked)
    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
        if (len < pos + 4) return std::nullopt;
        std::memcpy(mask_key, data + pos, 4);
        pos += 4;
    }

    // Check if we have enough data for the payload
    if (len < pos + payload_len) return std::nullopt;

    // Extract and unmask payload
    std::string payload(reinterpret_cast<const char*>(data + pos),
                        static_cast<size_t>(payload_len));

    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= static_cast<char>(mask_key[i % 4]);
        }
    }

    frame frm;
    frm.fin = fin;
    frm.op = op;
    frm.masked = masked;
    frm.payload = std::move(payload);

    return parse_result{std::move(frm), pos + static_cast<size_t>(payload_len)};
}

// ---------------------------------------------------------------------------
// websocket_connection — manages an active WebSocket connection
// ---------------------------------------------------------------------------

class websocket_connection {
public:
    explicit websocket_connection(tcp::socket& sock) : sock_(sock) {}

    // Receive the next complete message (handles fragmentation + control frames).
    // Returns empty string on close or error.
    Task<std::string> receive() {
        std::string message;
        opcode msg_opcode = opcode::continuation;
        bool in_fragment = false;

        while (true) {
            auto frm = co_await read_frame();
            if (!frm.has_value()) {
                co_return "";  // Connection closed or error
            }

            switch (frm->op) {
                case opcode::close: {
                    // Send close frame back
                    co_await send_close_frame();
                    co_return "";
                }

                case opcode::ping: {
                    // Respond with pong
                    auto pong_data = encode_frame(opcode::pong, frm->payload);
                    co_await write_raw(pong_data);
                    continue;  // Read next frame
                }

                case opcode::pong:
                    continue;  // Ignore unsolicited pongs

                case opcode::text:
                case opcode::binary:
                    if (frm->fin) {
                        // Complete single-frame message
                        co_return frm->payload;
                    }
                    // Start of fragmented message
                    msg_opcode = frm->op;
                    message = frm->payload;
                    in_fragment = true;
                    break;

                case opcode::continuation:
                    if (!in_fragment) {
                        co_return "";  // Protocol error
                    }
                    message += frm->payload;
                    if (frm->fin) {
                        in_fragment = false;
                        co_return message;
                    }
                    break;
            }
        }
    }

    // Send a text message
    Task<bool> send(std::string_view msg) {
        auto data = encode_frame(opcode::text, std::string(msg));
        co_return co_await write_raw(data);
    }

    // Send a binary message
    Task<bool> send_binary(const void* data, size_t len) {
        std::string payload(reinterpret_cast<const char*>(data), len);
        auto frame_data = encode_frame(opcode::binary, payload);
        co_return co_await write_raw(frame_data);
    }

    // Send a binary message from string
    Task<bool> send_binary(std::string_view data) {
        co_return co_await send_binary(data.data(), data.size());
    }

    // Send a close frame
    Task<void> close(uint16_t code = 1000, const std::string& reason = "") {
        std::string payload;
        payload += static_cast<char>((code >> 8) & 0xFF);
        payload += static_cast<char>(code & 0xFF);
        payload += reason;
        auto data = encode_frame(opcode::close, payload);
        co_await write_raw(data);
    }

    // Send a ping
    Task<bool> ping(std::string_view data = "") {
        auto frame_data = encode_frame(opcode::ping, std::string(data));
        co_return co_await write_raw(frame_data);
    }

private:
    tcp::socket& sock_;
    std::string read_buffer_;  // Accumulation buffer for partial reads

    // Read a complete frame from the socket
    Task<std::optional<frame>> read_frame() {
        char buf[4096];

        while (true) {
            // Try to parse from existing buffer
            if (!read_buffer_.empty()) {
                auto result = parse_frame(
                    reinterpret_cast<const uint8_t*>(read_buffer_.data()),
                    read_buffer_.size());
                if (result.has_value()) {
                    frame frm = std::move(result->frm);
                    read_buffer_.erase(0, result->consumed);
                    co_return frm;
                }
            }

            // Need more data
            auto n = co_await sock_.async_read_some(
                mutable_buffer(buf, sizeof(buf)));
            if (n <= 0) {
                co_return std::nullopt;  // Connection closed
            }
            read_buffer_.append(buf, static_cast<size_t>(n));
        }
    }

    // Write raw bytes to socket
    Task<bool> write_raw(const std::string& data) {
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            auto n = co_await sock_.async_write_some(
                const_buffer(data.data() + total_sent, data.size() - total_sent));
            if (n <= 0) co_return false;
            total_sent += static_cast<size_t>(n);
        }
        co_return true;
    }

    Task<void> send_close_frame() {
        auto data = encode_frame(opcode::close, "");
        co_await write_raw(data);
    }
};

// Handler type for WebSocket connections
using ws_handler_fn = std::function<Task<void>(websocket_connection&)>;

}  // namespace async_net::http::ws
