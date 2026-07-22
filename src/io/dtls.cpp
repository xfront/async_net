#include <async_net/io/dtls.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include "../security/dtls_backend.hpp"

namespace async_net::net {

dtls_stream::dtls_stream(int fd, ssl::context& ctx, bool is_server)
    : fd_(fd), is_server_(is_server) {
    handle_ = dtls_backend::create(ctx.native_handle(), fd, is_server);
}

dtls_stream::~dtls_stream() {
    dtls_backend::destroy(handle_);
}

dtls_stream::dtls_stream(dtls_stream&& other) noexcept
    : fd_(other.fd_)
    , is_server_(other.is_server_)
    , handle_(other.handle_) {
    other.fd_ = -1;
    other.handle_ = nullptr;
}

dtls_stream& dtls_stream::operator=(dtls_stream&& other) noexcept {
    if (this != &other) {
        dtls_backend::destroy(handle_);
        fd_ = other.fd_;
        is_server_ = other.is_server_;
        handle_ = other.handle_;
        other.fd_ = -1;
        other.handle_ = nullptr;
    }
    return *this;
}

void dtls_stream::begin_handshake() {
    dtls_backend::begin_handshake(handle_);
}

int dtls_stream::handshake_step() {
    int ret = dtls_backend::handshake_step(handle_);
    // Map backend normalized codes to caller-expected codes
    // Caller expects: 0=success, positive=WANT_READ/WANT_WRITE, negative=error
    if (ret == dtls_backend::OK) return 0;
    if (ret == dtls_backend::WANT_READ || ret == dtls_backend::WANT_WRITE) return ret;
    return -1;
}

int dtls_stream::handshake() {
    return dtls_backend::handshake(handle_, fd_) == 0 ? 0 : -1;
}

int dtls_stream::set_peer_from_socket() {
    return dtls_backend::set_peer_from_socket(handle_, fd_);
}

int dtls_stream::read(void* buf, size_t len) {
    return dtls_backend::read(handle_, buf, len);
}

int dtls_stream::write(const void* buf, size_t len) {
    return dtls_backend::write(handle_, buf, len);
}

void dtls_stream::shutdown() {
    dtls_backend::shutdown(handle_);
}

int dtls_stream::set_peer(const char* ip, uint16_t port) {
    return dtls_backend::set_peer(handle_, ip, port);
}

bool dtls_stream::wants_read() const {
    return dtls_backend::wants_read(handle_);
}

bool dtls_stream::wants_write() const {
    return dtls_backend::wants_write(handle_);
}

} // namespace async_net::net

#else

// Stub implementation when SSL is not available
namespace async_net::net {

dtls_stream::dtls_stream(int, ssl::context&, bool) {}
dtls_stream::~dtls_stream() {}
dtls_stream::dtls_stream(dtls_stream&&) noexcept {}
dtls_stream& dtls_stream::operator=(dtls_stream&&) noexcept { return *this; }
void dtls_stream::begin_handshake() {}
int dtls_stream::handshake_step() { return -1; }
int dtls_stream::handshake() { return -1; }
int dtls_stream::set_peer_from_socket() { return -1; }
int dtls_stream::read(void*, size_t) { return -1; }
int dtls_stream::write(const void*, size_t) { return -1; }
void dtls_stream::shutdown() {}
int dtls_stream::set_peer(const char*, uint16_t) { return -1; }
bool dtls_stream::wants_read() const { return false; }
bool dtls_stream::wants_write() const { return false; }

} // namespace async_net::net

#endif // ASYNC_NET_HAS_SSL
