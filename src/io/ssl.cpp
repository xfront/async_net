#include <async_net/net/ssl.hpp>

#ifdef ASYNC_NET_HAS_SSL

// wolfSSL headers (replaces OpenSSL)
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/openssl/asn1.h>

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
    static bool wolfssl_init = false;
    if (!wolfssl_init) {
        wolfSSL_Init();
        wolfssl_init = true;
    }

    WOLFSSL_METHOD* m = nullptr;
    std::string mstr(method);
    if (mstr == "tls_server") {
        m = wolfSSLv23_server_method();
    } else if (mstr == "tls_client") {
        m = wolfSSLv23_client_method();
    } else if (mstr == "dtls_server") {
        m = wolfDTLS_server_method();
    } else if (mstr == "dtls_client") {
        m = wolfDTLS_client_method();
    } else if (mstr == "dtls") {
        m = wolfDTLS_method();
    } else {
        m = wolfSSLv23_method();
    }

    ctx_ = wolfSSL_CTX_new(m);
}

context::~context() {
    if (ctx_) {
        wolfSSL_CTX_free(ctx_);
    }
}

context::context(context&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

context& context::operator=(context&& other) noexcept {
    if (this != &other) {
        if (ctx_) wolfSSL_CTX_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

bool context::use_certificate_file(const char* path) {
    return wolfSSL_CTX_use_certificate_chain_file(ctx_, path) == WOLFSSL_SUCCESS;
}

bool context::use_private_key_file(const char* path) {
    return wolfSSL_CTX_use_PrivateKey_file(ctx_, path, WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS;
}

bool context::load_verify_file(const char* path) {
    return wolfSSL_CTX_load_verify_locations(ctx_, path, nullptr) == WOLFSSL_SUCCESS;
}

void context::set_cipher_list(const char* ciphers) {
    wolfSSL_CTX_set_cipher_list(ctx_, ciphers);
}

void context::set_verify_peer(bool verify) {
    wolfSSL_CTX_set_verify(ctx_,
        verify ? WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT : WOLFSSL_VERIFY_NONE,
        nullptr);
}

void context::set_alpn_protos(const std::vector<std::string>& protos) {
    // Build ALPN wire format: length-prefixed strings
    std::vector<unsigned char> wire;
    for (auto& p : protos) {
        wire.push_back(static_cast<unsigned char>(p.size()));
        wire.insert(wire.end(), p.begin(), p.end());
    }
    wolfSSL_CTX_set_alpn_protos(ctx_, wire.data(), static_cast<unsigned int>(wire.size()));
}

// Server-side ALPN select callback (static) — wolfSSL convention:
// return 0 for match found, non-zero for no match
static int alpn_select_cb(WOLFSSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                           const unsigned char* in, unsigned int inlen, void* arg) {
    auto* user_cb = static_cast<std::function<std::string(const std::vector<std::string>&)>*>(arg);
    if (!user_cb) return 1; // no match

    // Parse client's ALPN list
    std::vector<std::string> client_protos;
    unsigned int pos = 0;
    while (pos < inlen) {
        unsigned int len = in[pos++];
        if (pos + len > inlen) break;
        client_protos.emplace_back(reinterpret_cast<const char*>(in + pos), len);
        pos += len;
    }

    std::string selected = (*user_cb)(client_protos);
    if (selected.empty()) return 1; // no match

    // Find the selected protocol in the wire data
    pos = 0;
    while (pos < inlen) {
        unsigned int len = in[pos];
        if (pos + 1 + len > inlen) break;
        std::string proto(reinterpret_cast<const char*>(in + pos + 1), len);
        if (proto == selected) {
            *out = in + pos + 1;
            *outlen = static_cast<unsigned char>(len);
            return 0; // match
        }
        pos += 1 + len;
    }

    return 1; // no match
}

void context::set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)> cb) {
    // Store callback in WOLFSSL_CTX app data
    auto* stored_cb = new std::function<std::string(const std::vector<std::string>&)>(std::move(cb));
    wolfSSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, stored_cb);
    // Note: this leaks the callback. A proper impl would store in ctx_ and clean up in destructor.
    // For now, this is acceptable for long-lived contexts.
}

// ---------------------------------------------------------------------------
// stream implementation — loop-based backend-integrated SSL
//
// Each SSL operation uses a simple loop:
//   1. Call the wolfSSL function (non-blocking)
//   2. On WANT_READ/WANT_WRITE: co_await a backend wait, then retry
//   3. On success or real error: co_return the result
// ---------------------------------------------------------------------------

stream::stream(tcp::socket& sock, context& ctx, bool is_server)
    : sock_(&sock), is_server_(is_server) {
    ssl_ = wolfSSL_new(ctx.native_handle());
    if (ssl_) {
        wolfSSL_set_fd(ssl_, static_cast<int>(sock.native_handle()));
    }
}

stream::~stream() {
    if (ssl_) {
        wolfSSL_free(ssl_);
    }
}

stream::stream(stream&& other) noexcept
    : ssl_(std::exchange(other.ssl_, nullptr))
    , sock_(other.sock_)
    , is_server_(other.is_server_) {}

stream& stream::operator=(stream&& other) noexcept {
    if (this != &other) {
        if (ssl_) wolfSSL_free(ssl_);
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

// Helper: drain and print wolfSSL error queue
static void drain_ssl_errors(const char* prefix) {
    unsigned long sslerr;
    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    while ((sslerr = wolfSSL_ERR_get_error()) != 0) {
        wolfSSL_ERR_error_string_n(sslerr, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "[%s] %s\n", prefix, errbuf);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// async_handshake — loop-based
// ---------------------------------------------------------------------------

Task<int> stream::async_handshake() {
    if (is_server_) {
        wolfSSL_set_accept_state(ssl_);
    } else {
        wolfSSL_set_connect_state(ssl_);
    }

    socket_t fd = sock_->native_handle();

    for (int attempt = 0; attempt < 100; ++attempt) {
        int ret = wolfSSL_SSL_do_handshake(ssl_);
        int err = wolfSSL_get_error(ssl_, ret);

        if (err == WOLFSSL_ERROR_NONE) {
            co_return 1; // Success
        }

        if (err == WOLFSSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == WOLFSSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue;
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
        int ret = wolfSSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = wolfSSL_get_error(ssl_, ret);

        if (err == WOLFSSL_ERROR_NONE) {
            co_return static_cast<ssize_t>(ret);
        }

        if (err == WOLFSSL_ERROR_ZERO_RETURN) {
            co_return 0; // Clean shutdown
        }

        if (err == WOLFSSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == WOLFSSL_ERROR_WANT_WRITE) {
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
        int ret = wolfSSL_write(ssl_, buf.data(), static_cast<int>(buf.size()));
        int err = wolfSSL_get_error(ssl_, ret);

        if (err == WOLFSSL_ERROR_NONE) {
            co_return static_cast<ssize_t>(ret);
        }

        if (err == WOLFSSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == WOLFSSL_ERROR_WANT_WRITE) {
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
        int ret = wolfSSL_shutdown(ssl_);

        if (ret >= 0) {
            co_return ret;
        }

        int err = wolfSSL_get_error(ssl_, ret);

        if (err == WOLFSSL_ERROR_WANT_READ) {
            co_await wait_for_socket(fd, true);
            continue;
        }
        if (err == WOLFSSL_ERROR_WANT_WRITE) {
            co_await wait_for_socket(fd, false);
            continue;
        }

        co_return -1;
    }

    co_return -1;
}

std::string stream::alpn_selected() const {
    const unsigned char* proto = nullptr;
    unsigned int len = 0;
    wolfSSL_get0_alpn_selected(ssl_, &proto, &len);
    if (proto && len > 0) {
        return std::string(reinterpret_cast<const char*>(proto), len);
    }
    return {};
}

} // namespace ssl
} // namespace async_net

#endif // ASYNC_NET_HAS_SSL
