#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace async_net::p2p {

/// Information about a peer registered with the tracker.
struct peer_info {
    std::string id;              ///< Unique identifier (e.g. "alice", UUID)
    std::string public_address;  ///< Public IP observed by tracker
    uint16_t udp_port = 0;      ///< UDP listening port
    uint16_t tcp_port = 0;      ///< TCP listening port (optional, reserved)
    std::chrono::steady_clock::time_point last_seen;

    /// Returns "ip:port" string for the UDP endpoint.
    std::string endpoint_str() const {
        return public_address + ":" + std::to_string(udp_port);
    }
};

/// P2P message frame types.
enum class msg_type : uint8_t {
    ping      = 1,   ///< Hole-punch probe / keepalive
    pong      = 2,   ///< Response to ping
    handshake = 3,   ///< DTLS handshake initiator signal
    data      = 4,   ///< Application data
    bye       = 5,   ///< Graceful disconnect
};

/// Wire format: [type:1][length:4][payload:N]
/// Header size = 5 bytes.
struct peer_message {
    msg_type type;
    std::vector<uint8_t> payload;

    /// Serialize to wire format.
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(5 + payload.size());
        buf[0] = static_cast<uint8_t>(type);
        uint32_t len = static_cast<uint32_t>(payload.size());
        buf[1] = static_cast<uint8_t>((len >> 24) & 0xFF);
        buf[2] = static_cast<uint8_t>((len >> 16) & 0xFF);
        buf[3] = static_cast<uint8_t>((len >>  8) & 0xFF);
        buf[4] = static_cast<uint8_t>( len        & 0xFF);
        if (!payload.empty()) {
            std::copy(payload.begin(), payload.end(), buf.begin() + 5);
        }
        return buf;
    }

    /// Deserialize from wire format. Returns nullopt if data is too short.
    static std::optional<peer_message> deserialize(const uint8_t* data, size_t len) {
        if (len < 5) return std::nullopt;
        peer_message msg;
        msg.type = static_cast<msg_type>(data[0]);
        uint32_t plen = (static_cast<uint32_t>(data[1]) << 24) |
                        (static_cast<uint32_t>(data[2]) << 16) |
                        (static_cast<uint32_t>(data[3]) <<  8) |
                         static_cast<uint32_t>(data[4]);
        if (len < 5 + plen) return std::nullopt;
        msg.payload.assign(data + 5, data + 5 + plen);
        return msg;
    }

    /// Create a ping message.
    static peer_message make_ping() {
        return peer_message{msg_type::ping, {}};
    }

    /// Create a pong message.
    static peer_message make_pong() {
        return peer_message{msg_type::pong, {}};
    }

    /// Create a data message from a string.
    static peer_message make_data(const std::string& text) {
        return peer_message{msg_type::data,
            std::vector<uint8_t>(text.begin(), text.end())};
    }

    /// Create a bye message.
    static peer_message make_bye() {
        return peer_message{msg_type::bye, {}};
    }
};

} // namespace async_net::p2p
