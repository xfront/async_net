#include "http1_codec.hpp"
#include <charconv>
#include <stdexcept>
#include <cstring>

namespace async_net {
namespace http {

// ---------------------------------------------------------------------------
// Find \r\n\r\n
// ---------------------------------------------------------------------------

size_t http1_codec::find_header_end(const std::string& data) {
    auto pos = data.find("\r\n\r\n");
    if (pos == std::string::npos) return std::string::npos;
    return pos + 4; // past the \r\n\r\n
}

// ---------------------------------------------------------------------------
// Parse header lines into headers object
// ---------------------------------------------------------------------------

void http1_codec::parse_header_lines(const std::string& block, headers& hdrs) {
    size_t pos = 0;
    while (pos < block.size()) {
        auto line_end = block.find("\r\n", pos);
        if (line_end == std::string::npos || line_end == pos) break;

        std::string line = block.substr(pos, line_end - pos);
        pos = line_end + 2;

        // Handle header folding (continuation with space/tab) — rare but spec'd
        while (pos < block.size() && (block[pos] == ' ' || block[pos] == '\t')) {
            auto next_end = block.find("\r\n", pos);
            if (next_end == std::string::npos) break;
            line += " " + block.substr(pos + 1, next_end - pos - 1);
            pos = next_end + 2;
        }

        // Split on first ':'
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim leading whitespace from value
        size_t start = 0;
        while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
            ++start;
        value = value.substr(start);

        // Trim trailing whitespace
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();

        hdrs.append(name, value);
    }
}

// ---------------------------------------------------------------------------
// Parse body (Content-Length or chunked)
// ---------------------------------------------------------------------------

http1_codec::body_result http1_codec::parse_body(const std::string& data, const headers& hdrs) {
    body_result result;

    if (hdrs.is_chunked()) {
        // Chunked transfer encoding
        size_t pos = 0;
        while (pos < data.size()) {
            // Read chunk size line
            auto line_end = data.find("\r\n", pos);
            if (line_end == std::string::npos) {
                return {result.data, pos, false}; // incomplete
            }

            std::string size_str = data.substr(pos, line_end - pos);
            size_t chunk_size = 0;
            auto [ptr, ec] = std::from_chars(size_str.data(), size_str.data() + size_str.size(),
                                              chunk_size, 16);
            if (ec != std::errc()) {
                throw std::runtime_error("Invalid chunk size");
            }

            pos = line_end + 2; // past \r\n

            if (chunk_size == 0) {
                // Terminal chunk — skip trailing \r\n
                if (pos + 2 <= data.size()) {
                    pos += 2;
                    result.consumed = pos;
                    result.complete = true;
                    return result;
                }
                return {result.data, pos, false};
            }

            // Read chunk data
            if (pos + chunk_size + 2 > data.size()) {
                return {result.data, 0, false}; // not enough data
            }

            result.data += data.substr(pos, chunk_size);
            pos += chunk_size + 2; // chunk data + \r\n
        }
        return {result.data, 0, false};
    }

    auto content_length = hdrs.content_length();
    if (content_length.has_value()) {
        int64_t len = *content_length;
        if (static_cast<int64_t>(data.size()) < len) {
            return {result.data, 0, false}; // not enough data
        }
        result.data = data.substr(0, static_cast<size_t>(len));
        result.consumed = static_cast<size_t>(len);
        result.complete = true;
        return result;
    }

    // No Content-Length, no chunked — no body (for requests)
    result.consumed = 0;
    result.complete = true;
    return result;
}

// ---------------------------------------------------------------------------
// Parse request (server side)
// ---------------------------------------------------------------------------

std::optional<std::pair<request, size_t>> http1_codec::parse_request(const std::string& data) {
    size_t header_end = find_header_end(data);
    if (header_end == std::string::npos) {
        return std::nullopt; // need more data
    }

    // Parse request line: METHOD PATH HTTP/x.x\r\n
    auto first_line_end = data.find("\r\n");
    if (first_line_end == std::string::npos) return std::nullopt;

    std::string request_line = data.substr(0, first_line_end);

    // Split by space
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) throw std::runtime_error("Invalid request line");
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) throw std::runtime_error("Invalid request line");

    std::string method_str = request_line.substr(0, sp1);
    std::string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version_str = request_line.substr(sp2 + 1);

    auto m = parse_method(method_str);
    if (!m) throw std::runtime_error("Unknown HTTP method: " + method_str);

    request req;
    req.method = *m;
    // Split path and query string
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        req.path = path.substr(0, qpos);
        req.query = path.substr(qpos + 1);
    } else {
        req.path = path;
    }
    if (req.path.empty()) req.path = "/";
    req.ver = (version_str == "HTTP/1.0") ? version::HTTP_10 : version::HTTP_11;

    // Parse headers (between request line and \r\n\r\n)
    std::string header_block = data.substr(first_line_end + 2, header_end - first_line_end - 4);
    parse_header_lines(header_block, req.hdrs);

    // Parse body
    std::string body_data = data.substr(header_end);
    auto body_res = parse_body(body_data, req.hdrs);
    if (!body_res.complete) {
        return std::nullopt; // body incomplete
    }

    req.bd = http::body(std::move(body_res.data));

    size_t total_consumed = header_end + body_res.consumed;
    return std::make_pair(std::move(req), total_consumed);
}

// ---------------------------------------------------------------------------
// Parse response (client side)
// ---------------------------------------------------------------------------

std::optional<std::pair<response, size_t>> http1_codec::parse_response(const std::string& data) {
    size_t header_end = find_header_end(data);
    if (header_end == std::string::npos) {
        return std::nullopt;
    }

    // Parse status line: HTTP/x.x CODE REASON\r\n
    auto first_line_end = data.find("\r\n");
    if (first_line_end == std::string::npos) return std::nullopt;

    std::string status_line = data.substr(0, first_line_end);

    auto sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) throw std::runtime_error("Invalid status line");

    std::string version_str = status_line.substr(0, sp1);
    std::string rest = status_line.substr(sp1 + 1);

    auto sp2 = rest.find(' ');
    std::string code_str = (sp2 != std::string::npos) ? rest.substr(0, sp2) : rest;

    int code = 0;
    std::from_chars(code_str.data(), code_str.data() + code_str.size(), code);
    if (code < 100 || code > 599) throw std::runtime_error("Invalid status code");

    response resp;
    resp.status = status_code(code);
    resp.ver = (version_str == "HTTP/1.0") ? version::HTTP_10 : version::HTTP_11;

    // Parse headers
    std::string header_block = data.substr(first_line_end + 2, header_end - first_line_end - 4);
    parse_header_lines(header_block, resp.hdrs);

    // Parse body
    std::string body_data = data.substr(header_end);

    // For responses: if no Content-Length and no chunked, read until connection close
    // In our model, we use Content-Length or chunked; connection-close is handled at transport
    if (!resp.hdrs.content_length().has_value() && !resp.hdrs.is_chunked()) {
        // Check if this is a response that should have no body
        if (code == 204 || code == 304 || (code >= 100 && code < 200)) {
            resp.bd = http::body();
            return std::make_pair(std::move(resp), header_end);
        }
        // No Content-Length and not chunked — might be connection close
        // For now, treat as no body if no data after headers
        if (body_data.empty()) {
            return std::nullopt; // need more data or connection close
        }
        // Otherwise, consume whatever is available
        resp.bd = http::body(std::move(body_data));
        return std::make_pair(std::move(resp), data.size());
    }

    auto body_res = parse_body(body_data, resp.hdrs);
    if (!body_res.complete) {
        return std::nullopt;
    }

    resp.bd = http::body(std::move(body_res.data));
    size_t total_consumed = header_end + body_res.consumed;
    return std::make_pair(std::move(resp), total_consumed);
}

// ---------------------------------------------------------------------------
// Serialize request
// ---------------------------------------------------------------------------

std::string http1_codec::serialize(const request& req) {
    std::string result;
    result += to_string(req.method);
    result += " ";
    result += req.path;
    result += " ";
    result += to_string(req.ver);
    result += "\r\n";
    result += req.hdrs.serialize();
    result += "\r\n";
    if (!req.bd.empty()) {
        result += req.bd.data();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Serialize response
// ---------------------------------------------------------------------------

std::string http1_codec::serialize(const response& resp) {
    std::string result;
    result += to_string(resp.ver);
    result += " ";
    result += std::to_string(resp.status.as_int());
    result += " ";
    result += resp.status.reason_phrase();
    result += "\r\n";
    result += resp.hdrs.serialize();
    result += "\r\n";
    if (!resp.bd.empty()) {
        result += resp.bd.data();
    }
    return result;
}

} // namespace http
} // namespace async_net
