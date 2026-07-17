#pragma once

#include <system_error>
#include <string>

namespace async_net {

class error_code {
public:
    error_code() : value_(0) {}
    explicit error_code(int val) : value_(val) {}

    int value() const { return value_; }
    explicit operator bool() const { return value_ != 0; }

    std::string message() const {
        return std::system_category().message(value_);
    }

    bool operator==(const error_code& other) const {
        return value_ == other.value_;
    }

    bool operator!=(const error_code& other) const {
        return value_ != other.value_;
    }

private:
    int value_;
};

// Common error codes
namespace errc {
    inline error_code success() { return error_code(0); }
    inline error_code connection_refused() { return error_code(ECONNREFUSED); }
    inline error_code connection_reset() { return error_code(ECONNRESET); }
    inline error_code broken_pipe() { 
#ifdef _WIN32
        return error_code(WSAECONNRESET);
#else
        return error_code(EPIPE);
#endif
    }
    inline error_code would_block() {
#ifdef _WIN32
        return error_code(WSAEWOULDBLOCK);
#else
        return error_code(EWOULDBLOCK);
#endif
    }
}

} // namespace async_net
