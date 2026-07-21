#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace async_net::http {

// ---------------------------------------------------------------------------
// HTTP Method
// ---------------------------------------------------------------------------

enum class method {
    GET, HEAD, POST, PUT, DELETE_, PATCH, OPTIONS, TRACE, CONNECT
};

inline const char* to_string(method m) {
    switch (m) {
        case method::GET:     return "GET";
        case method::HEAD:    return "HEAD";
        case method::POST:    return "POST";
        case method::PUT:     return "PUT";
        case method::DELETE_: return "DELETE";
        case method::PATCH:   return "PATCH";
        case method::OPTIONS: return "OPTIONS";
        case method::TRACE:   return "TRACE";
        case method::CONNECT: return "CONNECT";
    }
    return "UNKNOWN";
}

inline std::optional<method> parse_method(const std::string& s) {
    if (s == "GET")     return method::GET;
    if (s == "HEAD")    return method::HEAD;
    if (s == "POST")    return method::POST;
    if (s == "PUT")     return method::PUT;
    if (s == "DELETE")  return method::DELETE_;
    if (s == "PATCH")   return method::PATCH;
    if (s == "OPTIONS") return method::OPTIONS;
    if (s == "TRACE")   return method::TRACE;
    if (s == "CONNECT") return method::CONNECT;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// HTTP Version
// ---------------------------------------------------------------------------

enum class version { HTTP_10, HTTP_11, HTTP_2, HTTP_3 };

inline const char* to_string(version v) {
    switch (v) {
        case version::HTTP_10: return "HTTP/1.0";
        case version::HTTP_11: return "HTTP/1.1";
        case version::HTTP_2:  return "HTTP/2";
        case version::HTTP_3:  return "HTTP/3";
    }
    return "HTTP/1.1";
}

// ---------------------------------------------------------------------------
// HTTP Status Code
// ---------------------------------------------------------------------------

class status_code {
public:
    constexpr status_code() noexcept : code_(200) {}
    constexpr explicit status_code(int code) noexcept : code_(code) {}

    constexpr int as_int() const noexcept { return code_; }
    constexpr bool is_success() const noexcept { return code_ >= 200 && code_ < 300; }
    constexpr bool is_redirect() const noexcept { return code_ >= 300 && code_ < 400; }
    constexpr bool is_client_error() const noexcept { return code_ >= 400 && code_ < 500; }
    constexpr bool is_server_error() const noexcept { return code_ >= 500 && code_ < 600; }

    const char* reason_phrase() const {
        switch (code_) {
            case 100: return "Continue";
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 413: return "Payload Too Large";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }

    constexpr bool operator==(const status_code& o) const noexcept { return code_ == o.code_; }
    constexpr bool operator!=(const status_code& o) const noexcept { return code_ != o.code_; }

    // Common constants
    static constexpr status_code ok()                  { return status_code(200); }
    static constexpr status_code created()             { return status_code(201); }
    static constexpr status_code no_content()          { return status_code(204); }
    static constexpr status_code moved_permanently()   { return status_code(301); }
    static constexpr status_code found()               { return status_code(302); }
    static constexpr status_code bad_request()         { return status_code(400); }
    static constexpr status_code not_found()           { return status_code(404); }
    static constexpr status_code internal_error()      { return status_code(500); }

private:
    int code_;
};

// ---------------------------------------------------------------------------
// URI
// ---------------------------------------------------------------------------

class uri {
public:
    uri() = default;

    explicit uri(const std::string& url) { parse(url); }

    void parse(const std::string& url) {
        raw_ = url;
        std::string rest = url;

        // Scheme
        auto pos = rest.find("://");
        if (pos != std::string::npos) {
            scheme_ = rest.substr(0, pos);
            rest = rest.substr(pos + 3);
        } else {
            scheme_ = "http";
        }

        // Authority (host:port)
        pos = rest.find('/');
        std::string authority;
        if (pos != std::string::npos) {
            authority = rest.substr(0, pos);
            rest = rest.substr(pos);
        } else {
            authority = rest;
            rest = "/";
        }

        // Port
        auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host_ = authority.substr(0, colon);
            std::string port_str = authority.substr(colon + 1);
            int p = 0;
            std::from_chars(port_str.data(), port_str.data() + port_str.size(), p);
            port_ = static_cast<uint16_t>(p);
        } else {
            host_ = authority;
            port_ = (scheme_ == "https" || scheme_ == "h3") ? 443 : 80;
        }

        // Path + query + fragment
        pos = rest.find('#');
        std::string path_query = rest;
        if (pos != std::string::npos) {
            fragment_ = rest.substr(pos + 1);
            path_query = rest.substr(0, pos);
        }

        pos = path_query.find('?');
        if (pos != std::string::npos) {
            query_ = path_query.substr(pos + 1);
            path_ = path_query.substr(0, pos);
        } else {
            path_ = path_query;
        }

        if (path_.empty()) path_ = "/";
    }

    const std::string& scheme() const noexcept { return scheme_; }
    const std::string& host() const noexcept { return host_; }
    uint16_t port() const noexcept { return port_; }
    const std::string& path() const noexcept { return path_; }
    const std::string& query() const noexcept { return query_; }
    const std::string& fragment() const noexcept { return fragment_; }
    const std::string& raw() const noexcept { return raw_; }

    bool is_https() const noexcept { return scheme_ == "https"; }

    // host:port for connection
    std::string authority() const {
        return host_ + ":" + std::to_string(port_);
    }

private:
    std::string raw_;
    std::string scheme_ = "http";
    std::string host_;
    uint16_t port_ = 80;
    std::string path_ = "/";
    std::string query_;
    std::string fragment_;
};

// ---------------------------------------------------------------------------
// Headers — case-insensitive ordered container
// ---------------------------------------------------------------------------

class headers {
public:
    using value_type = std::pair<std::string, std::string>;
    using iterator = std::vector<value_type>::iterator;
    using const_iterator = std::vector<value_type>::const_iterator;

    headers() = default;

    // Case-insensitive set (replaces existing values)
    void set(const std::string& name, const std::string& value) {
        std::string lower = to_lower(name);
        // Remove all existing entries with this name
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                [&](const value_type& e) { return to_lower(e.first) == lower; }),
            entries_.end());
        entries_.emplace_back(name, value);
    }

    // Append (allows multiple values for same name)
    void append(const std::string& name, const std::string& value) {
        entries_.emplace_back(name, value);
    }

    // Get first value for name (case-insensitive)
    std::optional<std::string> get(const std::string& name) const {
        std::string lower = to_lower(name);
        for (auto& [k, v] : entries_) {
            if (to_lower(k) == lower) return v;
        }
        return std::nullopt;
    }

    // Get all values for name
    std::vector<std::string> get_all(const std::string& name) const {
        std::vector<std::string> result;
        std::string lower = to_lower(name);
        for (auto& [k, v] : entries_) {
            if (to_lower(k) == lower) result.push_back(v);
        }
        return result;
    }

    // Check if header exists
    bool contains(const std::string& name) const {
        return get(name).has_value();
    }

    // Remove all entries for name
    void remove(const std::string& name) {
        std::string lower = to_lower(name);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                [&](const value_type& e) { return to_lower(e.first) == lower; }),
            entries_.end());
    }

    // Content-Length helper
    std::optional<int64_t> content_length() const {
        auto val = get("Content-Length");
        if (!val) return std::nullopt;
        int64_t len = -1;
        std::from_chars(val->data(), val->data() + val->size(), len);
        return len >= 0 ? std::optional<int64_t>(len) : std::nullopt;
    }

    // Transfer-Encoding: chunked?
    bool is_chunked() const {
        auto val = get("Transfer-Encoding");
        if (!val) return false;
        std::string lower = to_lower(*val);
        return lower.find("chunked") != std::string::npos;
    }

    // Connection: keep-alive?
    bool is_keep_alive() const {
        auto val = get("Connection");
        if (!val) return true; // HTTP/1.1 default
        return to_lower(*val) == "keep-alive";
    }

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // Serialize to "Key: Value\r\n" lines
    std::string serialize() const {
        std::string result;
        for (auto& [k, v] : entries_) {
            result += k + ": " + v + "\r\n";
        }
        return result;
    }

private:
    std::vector<value_type> entries_;

    static std::string to_lower(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }
};

// ---------------------------------------------------------------------------
// Body
// ---------------------------------------------------------------------------

class body {
public:
    body() = default;
    explicit body(std::string data) : data_(std::move(data)) {}
    explicit body(const char* data) : data_(data) {}
    explicit body(const char* data, size_t len) : data_(data, len) {}

    const std::string& data() const noexcept { return data_; }
    std::string& data() noexcept { return data_; }
    bool empty() const noexcept { return data_.empty(); }
    size_t size() const noexcept { return data_.size(); }

    // Move the data out
    std::string take() { return std::move(data_); }

private:
    std::string data_;
};

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

struct request {
    http::method method = http::method::GET;
    std::string path = "/";
    std::string query;           // query string (after '?'), without the '?'
    http::version ver = http::version::HTTP_11;
    http::headers hdrs;
    http::body bd;

    // Helper: get a query parameter value by key
    std::string query_param(const std::string& key) const {
        auto p = query.find(key + "=");
        if (p == std::string::npos) return "";
        p += key.size() + 1;
        auto e = query.find('&', p);
        return query.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
};

// ---------------------------------------------------------------------------
// Request Builder
// ---------------------------------------------------------------------------

class request_builder {
public:
    request_builder& method_(http::method m) { req_.method = m; return *this; }
    request_builder& path(const std::string& p) { req_.path = p; return *this; }
    request_builder& version(http::version v) { req_.ver = v; return *this; }
    request_builder& header(const std::string& k, const std::string& v) {
        req_.hdrs.append(k, v);
        return *this;
    }
    request_builder& body(std::string b) {
        req_.bd = http::body(std::move(b));
        return *this;
    }
    request build() {
        if (!req_.bd.empty() && !req_.hdrs.contains("Content-Length")) {
            req_.hdrs.set("Content-Length", std::to_string(req_.bd.size()));
        }
        return std::move(req_);
    }
private:
    request req_;
};

inline request_builder request_make() { return request_builder{}; }

// Allow request::make() syntax via extension
inline request_builder request_make_builder() { return request_builder{}; }

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

struct response {
    http::status_code status = http::status_code::ok();
    http::version ver = http::version::HTTP_11;
    http::headers hdrs;
    http::body bd;
    http::headers trailers;  // HTTP/2 trailers (for gRPC support)
};

// ---------------------------------------------------------------------------
// Response Builder
// ---------------------------------------------------------------------------

class response_builder {
public:
    response_builder& status(http::status_code s) { resp_.status = s; return *this; }
    response_builder& version(http::version v) { resp_.ver = v; return *this; }
    response_builder& header(const std::string& k, const std::string& v) {
        resp_.hdrs.append(k, v);
        return *this;
    }
    response_builder& body(std::string b) {
        resp_.bd = http::body(std::move(b));
        return *this;
    }
    response build() {
        if (!resp_.bd.empty() && !resp_.hdrs.contains("Content-Length")) {
            resp_.hdrs.set("Content-Length", std::to_string(resp_.bd.size()));
        }
        if (!resp_.hdrs.contains("Content-Type")) {
            resp_.hdrs.set("Content-Type", "text/plain");
        }
        return std::move(resp_);
    }
private:
    response resp_;
};

inline response_builder response_make() { return response_builder{}; }

// Quick response factory helpers
inline response response_ok(std::string body_text = "") {
    return response_builder().status(status_code::ok()).body(std::move(body_text)).build();
}
inline response response_not_found() {
    return response_builder().status(status_code::not_found()).body("Not Found").build();
}
inline response response_bad_request(std::string msg = "Bad Request") {
    return response_builder().status(status_code::bad_request()).body(std::move(msg)).build();
}
inline response response_internal_error(std::string msg = "Internal Server Error") {
    return response_builder().status(status_code::internal_error()).body(std::move(msg)).build();
}

} // namespace async_net::http
