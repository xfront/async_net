#include <async_net/net/ssl.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <async_net/io/io_context.hpp>
#include <async_net/io/io_backend.hpp>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace async_net {
namespace ssl {

// ---------------------------------------------------------------------------
// context implementation
// ---------------------------------------------------------------------------

context::context(const char* method) {
    static bool openssl_init = false;
    if (!openssl_init) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        openssl_init = true;
    }

    const SSL_METHOD* m = nullptr;
    std::string mstr(method);
    if (mstr == "tls_server") {
        m = TLS_server_method();
    } else if (mstr == "tls_client") {
        m = TLS_client_method();
    } else {
        m = TLS_method();
    }

    ctx_ = SSL_CTX_new(m);
}

context::~context() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

context::context(context&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

context& context::operator=(context&& other) noexcept {
    if (this != &other) {
        if (ctx_) SSL_CTX_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

bool context::use_certificate_file(const char* path) {
    return SSL_CTX_use_certificate_chain_file(ctx_, path) == 1;
}

bool context::use_private_key_file(const char* path) {
    return SSL_CTX_use_PrivateKey_file(ctx_, path, SSL_FILETYPE_PEM) == 1;
}

bool context::load_verify_file(const char* path) {
    return SSL_CTX_load_verify_locations(ctx_, path, nullptr) == 1;
}

void context::set_cipher_list(const char* ciphers) {
    SSL_CTX_set_cipher_list(ctx_, ciphers);
}

void context::set_verify_peer(bool verify) {
    SSL_CTX_set_verify(ctx_,
        verify ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_NONE,
        nullptr);
}

// ---------------------------------------------------------------------------
// stream implementation — loop-based backend-integrated SSL
//
// Each SSL operation uses a simple loop:
//   1. Call the SSL function (non-blocking)
//   2. On WANT_READ/WANT_WRITE: co_await a backend wait, then retry
//   3. On success or real error: co_return the result
//
// The backend wait is a simple awaiter that submits a wait operation to
// the I/O backend (epoll/kqueue/io_uring/IOCP) and suspends until the
// socket is ready. No recursion, no complex handle management.
// ---------------------------------------------------------------------------

stream::stream(tcp::socket& sock, context& ctx, bool is_server)
    : sock_(&sock), is_server_(is_server) {
    ssl_ = SSL_new(ctx.native_handle());
    if (ssl_) {
        SSL_set_fd(ssl_, static_cast<int>(sock.native_handle()));
    }
}

stream::~stream() {
    if (ssl_) {
        SSL_free(ssl_);
    }
}

stream::stream(stream&& other) noexcept
    : ssl_(std::exchange(other.ssl_, nullptr))
    , sock_(other.sock_)
    , is_server_(other.is_server_) {}

stream& stream::operator=(stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) SSL_free(ssl_);
        ssl_ = std::exchange(other.ssl_, nullptr);
        sock_ = other.sock_;
        is_server_ = other.is_server_;
    }
    return *this;
}

// Simple awaiter: waits for socket readability or writability via the backend.
// Does NOT call any SSL function — just waits for the socket to be ready.
struct SocketWaitAwaiter {
    socket_t fd;
    bool wait_readable;
    std::shared_ptr<OperationContext> ctx;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        ctx = std::make_shared<OperationContext>();
        ctx->set_handle(h);

        auto* io_ctx = io_context::current();
        if (!io_ctx) return false; // Can't wait, resume immediately

        if (wait_readable) {
            ctx->set_type(OpType::WaitReadable);
            io_ctx->backend().async_wait_readable(fd, ctx);
        } else {
            ctx->set_type(OpType::WaitWritable);
            io_ctx->backend().async_wait_writable(fd, ctx);
        }

        // If the wait completed synchronously (e.g., data already available),
        // resume immediately
        return !ctx->completed();
    }

    void await_resume() const noexcept {}
};

static Task<void> wait_for_socket(socket_t fd, bool readable) {
    co_await SocketWaitAwaiter{fd, readable, nullptr};
}

// Helper: drain and print OpenSSL error queue
static void drain_ssl_errors(const char* prefix) {
    unsigned long sslerr;
    char errbuf[256];
    while ((sslerr = ERR_get_error()) != 0) {
        ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[%s] %s\n", prefix, errbuf);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// async_handshake — loop-based
// ---------------------------------------------------------------------------

Task<int> stream::async_handshake() {
    if (is_server_) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
    }

    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = SSL_do_handshake(ssl_);
        int err = SSL_get_error(ssl_, ret);

        if (err == SSL_ERROR_NONE) {
            co_return 1; // Success
        }

        if (err == SSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue; // Retry
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue; // Retry
        }

        // Fatal error
        drain_ssl_errors("SSL handshake");
        co_return -1;
    }

    co_return -1; // Too many retries
}

// ---------------------------------------------------------------------------
// async_read_some — loop-based
// ---------------------------------------------------------------------------

Task<ssize_t> stream::async_read_some(mutable_buffer buf) {
    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = SSL_get_error(ssl_, ret);

        if (err == SSL_ERROR_NONE) {
            co_return static_cast<ssize_t>(ret);
        }

        if (err == SSL_ERROR_ZERO_RETURN) {
            co_return 0; // Clean shutdown
        }

        if (err == SSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue;
        }

        drain_ssl_errors("SSL read");
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
        int ret = SSL_write(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = SSL_get_error(ssl_, ret);

        if (err == SSL_ERROR_NONE) {
            co_return static_cast<ssize_t>(ret);
        }

        if (err == SSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue;
        }

        drain_ssl_errors("SSL write");
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
        int ret = SSL_shutdown(ssl_);

        if (ret >= 0) {
            co_return ret; // 0 = unidirectional, 1 = bidirectional
        }

        int err = SSL_get_error(ssl_, ret);

        if (err == SSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue;
        }

        co_return -1;
    }

    co_return -1;
}

} // namespace ssl
} // namespace async_net

#endif // ASYNC_NET_HAS_SSL
