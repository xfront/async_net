#include <async_net/io/ssl.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include <async_net/io/io_context.hpp>
#include <async_net/io/io_backend.hpp>

namespace async_net::ssl {

// ===========================================================================
// SocketWaitAwaiter — coroutine awaiter for socket I/O readiness
// ===========================================================================

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

// ===========================================================================
// basic_context<P> — template implementation
// ===========================================================================

template<typename P>
basic_context<P>::basic_context(const char* method) {
    P::init();
    ctx_ = P::ctx_new(method);
}

template<typename P>
basic_context<P>::~basic_context() {
    if (ctx_) P::ctx_free(ctx_);
}

template<typename P>
basic_context<P>::basic_context(basic_context&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)) {}

template<typename P>
basic_context<P>& basic_context<P>::operator=(basic_context&& other) noexcept {
    if (this != &other) {
        if (ctx_) P::ctx_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

template<typename P>
bool basic_context<P>::use_certificate_file(const char* path) {
    return P::ctx_use_cert(ctx_, path);
}

template<typename P>
bool basic_context<P>::use_private_key_file(const char* path) {
    return P::ctx_use_key(ctx_, path);
}

template<typename P>
bool basic_context<P>::load_verify_file(const char* path) {
    return P::ctx_load_verify(ctx_, path);
}

template<typename P>
void basic_context<P>::set_cipher_list(const char* ciphers) {
    P::ctx_set_cipher_list(ctx_, ciphers);
}

template<typename P>
void basic_context<P>::set_verify_peer(bool verify) {
    P::ctx_set_verify(ctx_, verify);
}

template<typename P>
void basic_context<P>::set_alpn_protos(const std::vector<std::string>& protos) {
    std::vector<unsigned char> wire;
    for (auto& p : protos) {
        wire.push_back(static_cast<unsigned char>(p.size()));
        wire.insert(wire.end(), p.begin(), p.end());
    }
    P::ctx_set_alpn_protos(ctx_, wire.data(), static_cast<unsigned int>(wire.size()));
}

template<typename P>
void basic_context<P>::set_alpn_select_cb(
    std::function<std::string(const std::vector<std::string>&)> cb) {
    auto* stored_cb = new std::function<std::string(const std::vector<std::string>&)>(std::move(cb));
    P::ctx_set_alpn_select_cb(ctx_, stored_cb);
}

template class basic_context<>;

// ===========================================================================
// basic_stream<P> — template implementation
// ===========================================================================

template<typename P>
basic_stream<P>::basic_stream(tcp::socket& sock, basic_context<P>& ctx, bool is_server)
    : sock_(&sock), is_server_(is_server) {
    ssl_ = P::stream_new(ctx.native_handle(), static_cast<int>(sock.native_handle()));
}

template<typename P>
basic_stream<P>::~basic_stream() {
    if (ssl_) P::stream_free(ssl_);
}

template<typename P>
basic_stream<P>::basic_stream(basic_stream&& other) noexcept
    : ssl_(std::exchange(other.ssl_, nullptr))
    , sock_(other.sock_)
    , is_server_(other.is_server_) {}

template<typename P>
basic_stream<P>& basic_stream<P>::operator=(basic_stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) P::stream_free(ssl_);
        ssl_ = std::exchange(other.ssl_, nullptr);
        sock_ = other.sock_;
        is_server_ = other.is_server_;
    }
    return *this;
}

template<typename P>
Task<int> basic_stream<P>::async_handshake() {
    if (is_server_) P::stream_set_accept_state(ssl_);
    else            P::stream_set_connect_state(ssl_);

    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = P::stream_do_handshake(ssl_);
        int err = P::stream_get_error(ssl_, ret);

        if (err == ERR_NONE) co_return 1;
        if (err == ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        P::drain_errors("SSL handshake");
        co_return -1;
    }
    co_return -1;
}

template<typename P>
Task<ssize_t> basic_stream<P>::async_read_some(mutable_buffer buf) {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = P::stream_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = P::stream_get_error(ssl_, ret);

        if (err == ERR_NONE) co_return static_cast<ssize_t>(ret);
        if (err == ERR_ZERO_RETURN) co_return 0;
        if (err == ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        P::drain_errors("SSL read");
        co_return -1;
    }
    co_return -1;
}

template<typename P>
Task<ssize_t> basic_stream<P>::async_write_some(const_buffer buf) {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = P::stream_write(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = P::stream_get_error(ssl_, ret);

        if (err == ERR_NONE) co_return static_cast<ssize_t>(ret);
        if (err == ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }

        P::drain_errors("SSL write");
        co_return -1;
    }
    co_return -1;
}

template<typename P>
Task<int> basic_stream<P>::async_shutdown() {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 10; ++attempt) {
        int ret = P::stream_shutdown(ssl_);
        if (ret >= 0) co_return ret;

        int err = P::stream_get_error(ssl_, ret);

        if (err == ERR_WANT_READ)  { co_await wait_for_socket(fd, true);  continue; }
        if (err == ERR_WANT_WRITE) { co_await wait_for_socket(fd, false); continue; }
        co_return -1;
    }
    co_return -1;
}

template<typename P>
std::string basic_stream<P>::alpn_selected() const {
    return P::stream_alpn_selected(ssl_);
}

template class basic_stream<>;

} // namespace async_net::ssl

#endif // ASYNC_NET_HAS_SSL
