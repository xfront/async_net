#pragma once

#include <async_net/http/types.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace async_net::http {

// Result of a parse attempt
enum class parse_status {
    complete,   // Message fully parsed
    incomplete, // Need more data
    error       // Parse error
};

// ---------------------------------------------------------------------------
// HTTP/1.1 Codec — incremental parser and serializer
// ---------------------------------------------------------------------------

class http1_codec {
public:
    // --- Request parsing (server side) ---

    // Feed raw bytes and try to parse a request.
    // On success, returns the parsed request and number of bytes consumed.
    // On incomplete, returns nullopt (need more data).
    // On error, throws std::runtime_error.
    static std::optional<std::pair<request, size_t>> parse_request(const std::string& data);

    // --- Response parsing (client side) ---

    static std::optional<std::pair<response, size_t>> parse_response(const std::string& data);

    // --- Serialization ---

    static std::string serialize(const request& req);
    static std::string serialize(const response& resp);

private:
    // Internal: find \r\n\r\n header terminator
    static size_t find_header_end(const std::string& data);

    // Parse headers from string (lines between request/status line and \r\n\r\n)
    static void parse_header_lines(const std::string& header_block, headers& hdrs);

    // Parse body given headers (Content-Length or chunked)
    // Returns: {body_data, bytes_consumed, complete}
    struct body_result {
        std::string data;
        size_t consumed;
        bool complete;
    };
    static body_result parse_body(const std::string& data, const headers& hdrs);
};

} // namespace async_net::http
