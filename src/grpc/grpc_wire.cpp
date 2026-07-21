// gRPC Wire Protocol implementation
// Length-prefixed message framing: [1 byte flags][4 bytes length BE][N bytes payload]

#include <async_net/grpc/types.hpp>
#include <cstring>

namespace async_net::grpc {

std::string encode_grpc_message(const std::string& protobuf_data) {
    std::string frame;
    frame.resize(5 + protobuf_data.size());

    // Byte 0: compression flag (0 = no compression)
    frame[0] = '\x00';

    // Bytes 1-4: message length in big-endian
    uint32_t len = static_cast<uint32_t>(protobuf_data.size());
    frame[1] = static_cast<char>((len >> 24) & 0xFF);
    frame[2] = static_cast<char>((len >> 16) & 0xFF);
    frame[3] = static_cast<char>((len >>  8) & 0xFF);
    frame[4] = static_cast<char>((len >>  0) & 0xFF);

    // Bytes 5+: protobuf payload
    if (!protobuf_data.empty()) {
        std::memcpy(&frame[5], protobuf_data.data(), protobuf_data.size());
    }

    return frame;
}

std::optional<std::string> decode_grpc_message(const uint8_t* data, size_t len) {
    if (len < 5) return std::nullopt;

    // Byte 0: compression flag (we only support identity = 0)
    // uint8_t flags = data[0];

    // Bytes 1-4: message length in big-endian
    uint32_t msg_len = (static_cast<uint32_t>(data[1]) << 24)
                     | (static_cast<uint32_t>(data[2]) << 16)
                     | (static_cast<uint32_t>(data[3]) <<  8)
                     | (static_cast<uint32_t>(data[4]) <<  0);

    if (5 + msg_len > len) return std::nullopt;

    return std::string(reinterpret_cast<const char*>(data + 5), msg_len);
}

} // namespace async_net::grpc
