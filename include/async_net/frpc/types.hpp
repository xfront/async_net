// FRPC core types — status codes, constants, and FlatBuffers helpers
// Built on top of HTTP/2 transport, reusing gRPC wire framing.
#pragma once

#include <async_net/grpc/types.hpp>
#include <async_net/grpc/stream.hpp>
#include <async_net/grpc/stream_deframer.hpp>
#include "async_net/grpc/channel.hpp"

namespace async_net::frpc {

// Reuse gRPC core types (status, metadata, call_context, reader, writer, etc.)
using status_code = grpc::status_code;
using status = grpc::status;
using metadata = grpc::metadata;
using call_context = grpc::call_context;
using reader = grpc::reader;
using writer = grpc::writer;
using stream_deframer = grpc::stream_deframer;
using PromiseAwaiter = grpc::PromiseAwaiter;

// Reuse gRPC wire protocol helpers (same framing format)
using grpc::encode_grpc_message;
using grpc::decode_grpc_message;
using grpc::make_path;
using grpc::parse_path;

// ============================================================================
// FRPC HTTP/2 header constants
// ============================================================================

namespace frpc_constants {
    constexpr const char* content_type     = "application/grpc+flatbuffers";
    constexpr const char* grpc_encoding    = "identity";
    constexpr const char* te               = "trailers";

    // Trailer header names (same as gRPC)
    constexpr const char* grpc_status      = "grpc-status";
    constexpr const char* grpc_message     = "grpc-message";
}

// ============================================================================
// to_string for status_code — use grpc::to_string directly
// Users should call grpc::to_string(status.code) or use frpc::grpc::to_string
// ============================================================================

// Note: to_string is available via grpc namespace

} // namespace async_net::frpc
