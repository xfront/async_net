// gRPC core types — status codes, message framing, wire protocol helpers
// Built on top of HTTP/2 transport (application/grpc content type).
#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <map>
#include <async_net/http/types.hpp>

namespace async_net::grpc {

// ============================================================================
// gRPC Status Codes (standard)
// ============================================================================

enum class status_code : int {
    ok                  = 0,
    cancelled           = 1,
    unknown             = 2,
    invalid_argument    = 3,
    deadline_exceeded   = 4,
    not_found           = 5,
    already_exists      = 6,
    permission_denied   = 7,
    resource_exhausted  = 8,
    failed_precondition = 9,
    aborted             = 10,
    out_of_range        = 11,
    unimplemented       = 12,
    internal            = 13,
    unavailable         = 14,
    data_loss           = 15,
    unauthenticated     = 16,
};

inline const char* to_string(status_code code) {
    switch (code) {
        case status_code::ok:                  return "OK";
        case status_code::cancelled:           return "CANCELLED";
        case status_code::unknown:             return "UNKNOWN";
        case status_code::invalid_argument:    return "INVALID_ARGUMENT";
        case status_code::deadline_exceeded:   return "DEADLINE_EXCEEDED";
        case status_code::not_found:           return "NOT_FOUND";
        case status_code::already_exists:      return "ALREADY_EXISTS";
        case status_code::permission_denied:   return "PERMISSION_DENIED";
        case status_code::resource_exhausted:  return "RESOURCE_EXHAUSTED";
        case status_code::failed_precondition: return "FAILED_PRECONDITION";
        case status_code::aborted:             return "ABORTED";
        case status_code::out_of_range:        return "OUT_OF_RANGE";
        case status_code::unimplemented:       return "UNIMPLEMENTED";
        case status_code::internal:            return "INTERNAL";
        case status_code::unavailable:         return "UNAVAILABLE";
        case status_code::data_loss:           return "DATA_LOSS";
        case status_code::unauthenticated:     return "UNAUTHENTICATED";
        default:                               return "UNKNOWN";
    }
}

// ============================================================================
// gRPC Status
// ============================================================================

struct status {
    status_code code = status_code::ok;
    std::string message;

    bool ok() const { return code == status_code::ok; }
};

// ============================================================================
// gRPC Metadata (key-value pairs, like HTTP headers)
// ============================================================================

using metadata = std::map<std::string, std::string>;

// ============================================================================
// Call Context — carries metadata through the RPC call chain
// ============================================================================

struct call_context {
    // Initial metadata sent by the client with the request
    metadata initial_metadata;

    // Response metadata that the handler can populate (sent as trailing metadata)
    metadata response_metadata;

    // Extract metadata from HTTP/2 headers (non-pseudo, non-gRPC headers)
    static metadata extract_from_headers(const http::headers& hdrs) {
        metadata md;
        for (auto& [k, v] : hdrs) {
            // Skip pseudo-headers, standard gRPC headers, and HTTP/2 headers
            if (k.empty() || k[0] == ':' || k[0] == '-') continue;
            if (k == "content-type" || k == "te" || k == "host" ||
                k == "grpc-encoding" || k == "grpc-status" || k == "grpc-message" ||
                k == "grpc-accept-encoding" || k == "user-agent" ||
                k == "accept-encoding") continue;
            md[k] = v;
        }
        return md;
    }
};

// ============================================================================
// Wire Protocol: Length-prefixed message framing
//
// Each gRPC message is framed as:
//   [1 byte: compressed flag] [4 bytes: message length (big-endian)] [N bytes: message]
//
// For identity (no compression), compressed flag = 0.
// ============================================================================

// Encode a protobuf message into a gRPC frame (length-prefixed)
std::string encode_grpc_message(const std::string& protobuf_data);

// Decode a gRPC frame, returning the protobuf message data.
// Returns nullopt if the data is too short or malformed.
std::optional<std::string> decode_grpc_message(const uint8_t* data, size_t len);

// Decode from a std::string
inline std::optional<std::string> decode_grpc_message(const std::string& data) {
    return decode_grpc_message(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ============================================================================
// HTTP/2 header constants for gRPC
// ============================================================================

namespace grpc_constants {
    constexpr const char* content_type     = "application/grpc";
    constexpr const char* grpc_encoding    = "identity";
    constexpr const char* te               = "trailers";

    // Trailer header names
    constexpr const char* grpc_status      = "grpc-status";
    constexpr const char* grpc_message     = "grpc-message";
}

// ============================================================================
// Helper: build gRPC request path from service + method
// ============================================================================

inline std::string make_path(const std::string& service, const std::string& method) {
    return "/" + service + "/" + method;
}

// Parse service and method from a gRPC path like "/ServiceName/MethodName"
// Returns {service, method} or empty strings on failure.
inline std::pair<std::string, std::string> parse_path(const std::string& path) {
    if (path.empty() || path[0] != '/') return {"", ""};
    auto slash = path.find('/', 1);
    if (slash == std::string::npos) return {"", ""};
    return {path.substr(1, slash - 1), path.substr(slash + 1)};
}

} // namespace async_net::grpc
