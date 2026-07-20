#include <async_net/net/ssl.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include "../security/ssl_backend.hpp"
#include <async_net/io/io_context.hpp>
#include <async_net/io/io_backend.hpp>

namespace async_net::ssl {

// ---------------------------------------------------------------------------
// context implementation
// ---------------------------------------------------------------------------

context::context(const char* method) {
    backend::init();
    ctx_ = backend::ctx_new(method);
}

context::~context() {
    if (ctx_) backend::ctx_free(ctx_);
}

context::context(context&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

context& context::operator=(context&& other) noexcept {
    if (this != &other) {
        if (ctx_) backend::ctx_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

bool context::use_certificate_file(const char* path) {
    return backend::ctx_use_cert(ctx_, path);
}

bool context::use_private_key_file(const char* path) {
    return backend::ctx_use_key(ctx_, path);
}

bool context::load_verify_file(const char* path) {
    return backend::ctx_load_verify(ctx_, path);
}

void context::set_cipher_list(const char* ciphers) {
    backend::ctx_set_cipher_list(ctx_, ciphers);
}

void context::set_verify_peer(bool verify) {
    backend::ctx_set_verify(ctx_, verify);
}

void context::set_alpn_protos(const std::vector<std::string>& protos) {
    std::vector<unsigned char> wire;
    for (auto& p : protos) {
        wire.push_back(static_cast<unsigned char>(p.size()));
        wire.insert(wire.end(), p.begin(), p.end());
    }
    backend::ctx_set_alpn_protos(ctx_, wire.data(), static_cast<unsigned int>(wire.size()));
}

void context::set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)> cb) {
    auto* stored_cb = new std::function<std::string(const std::vector<std::string>&)>(std::move(cb));
    backend::ctx_set_alpn_select_cb(ctx_, stored_cb);
}

// ---------------------------------------------------------------------------
// stream implementation — loop-based backend-integrated SSL
//
// Each SSL operation uses a simple loop:
//   1. Call the SSL function (non-blocking)
//   2. On WANT_READ/WANT_WRITE: co_await a backend wait, then retry
//   3. On success or real error: co_return the result
// ---------------------------------------------------------------------------

stream::stream(tcp::socket& sock, context& ctx, bool is_server)
    : sock_(&sock), is_server_(is_server) {
    ssl_ = backend::stream_new(ctx.native_handle(), static_cast<int>(sock.native_handle()));
}

stream::~stream() {
    if (ssl_) backend::stream_free(ssl_);
}

stream::stream(stream&& other) noexcept
    : ssl_(std::exchange(other.ssl_, nullptr))
    , sock_(other.sock_)
    , is_server_(other.is_server_) {}

stream& stream::operator=(stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) backend::stream_free(ssl_);
        ssl_ = std::exchange(other.ssl_, nullptr);
        sock_ = other.sock_;
        is_server_ = other.is_server_;
    }
    return *this;
}

// Simple awaiter: waits for socket readability or writability via the backend.
struct SocketWaitAwaiter {
    socket_t fd;
    bool wait_readable;
    std::shared_ptr<OperationContext> ctx;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        ctx = std::make_shared<OperationContext>();
        ctx->set_handle(h);

        auto* io_ctx = io_context::current();
        if (!io_ctx) return false;

        if (wait_readable) {
            ctx->set_type(OpType::WaitReadable);
            io_ctx->backend().async_wait_readable(fd, ctx);
        } else {
            ctx->set_type(OpType::WaitWritable);
            io_ctx->backend().async_wait_writable(fd, ctx);
        }

        return !ctx->completed();
    }

    void await_resume() const noexcept {}
};

static Task<void> wait_for_socket(socket_t fd, bool readable) {
    co_await SocketWaitAwaiter{fd, readable, nullptr};
}

// ---------------------------------------------------------------------------
// async_handshake — loop-based
// ---------------------------------------------------------------------------

Task<int> stream::async_handshake() {
    if (is_server_) backend::stream_set_accept_state(ssl_);
    else            backend::stream_set_connect_state(ssl_);

    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = backend::stream_do_handshake(ssl_);
        int err = backend::stream_get_error(ssl_, ret);

        if (err == backend::ERR_NONE) co_return 1;
        if (err == backend::ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == backend::ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        backend::drain_errors("SSL handshake");
        co_return -1;
    }
    co_return -1;
}

// ---------------------------------------------------------------------------
// async_read_some — loop-based
// ---------------------------------------------------------------------------

Task<ssize_t> stream::async_read_some(mutable_buffer buf) {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = backend::stream_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = backend::stream_get_error(ssl_, ret);

        if (err == backend::ERR_NONE) co_return static_cast<ssize_t>(ret);
        if (err == backend::ERR_ZERO_RETURN) co_return 0;
        if (err == backend::ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == backend::ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        backend::drain_errors("SSL read");
        co_return -1;
    }
    co_return -1;
}

// ---------------------------------------------------------------------------
// async_write_some — loop-based
// ---------------------------------------------------------------------------

Task<ssize_t> stream::async_write_some(const_buffer buf) {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = backend::stream_write(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = backend::stream_get_error(ssl_, ret);

        if (err == backend::ERR_NONE) co_return static_cast<ssize_t>(ret);
        if (err == backend::ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == backend::ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        backend::drain_errors("SSL write");
        co_return -1;
    }
    co_return -1;
}

// ---------------------------------------------------------------------------
// async_shutdown — loop-based with retry limit
// ---------------------------------------------------------------------------

Task<int> stream::async_shutdown() {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 10; ++attempt) {
        int ret = backend::stream_shutdown(ssl_);
        if (ret >= 0) co_return ret;

        int err = backend::stream_get_error(ssl_, ret);

        if (err == backend::ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == backend::ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }
        co_return -1;
    }
    co_return -1;
}

std::string stream::alpn_selected() const {
    return backend::stream_alpn_selected(ssl_);
}

} // namespace async_net::ssl

#else

// Stub — no SSL
namespace async_net::ssl {
context::context(const char*) {}
context::~context() {}
context::context(context&&) noexcept {}
context& context::operator=(context&&) noexcept { return *this; }
bool context::use_certificate_file(const char*) { return false; }
bool context::use_private_key_file(const char*) { return false; }
bool context::load_verify_file(const char*) { return false; }
void context::set_cipher_list(const char*) {}
void context::set_verify_peer(bool) {}
void context::set_alpn_protos(const std::vector<std::string>&) {}
void context::set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)>) {}
stream::stream(tcp::socket&, context&, bool) {}
stream::~stream() {}
stream::stream(stream&&) noexcept {}
stream& stream::operator=(stream&&) noexcept { return *this; }
Task<int> stream::async_handshake() { co_return -1; }
Task<ssize_t> stream::async_read_some(mutable_buffer) { co_return -1; }
Task<ssize_t> stream::async_write_some(const_buffer) { co_return -1; }
Task<int> stream::async_shutdown() { co_return -1; }
std::string stream::alpn_selected() const { return {}; }
} // namespace async_net::ssl

#endif // ASYNC_NET_HAS_SSL
