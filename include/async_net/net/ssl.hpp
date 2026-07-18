#pragma once

#include "../detail/config.hpp"
#include "tcp.hpp"
#include "../coroutine/task.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

// wolfSSL headers (replaces OpenSSL)
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

namespace async_net {
namespace ssl {

/// SSL/TLS context — holds certificates, keys, and configuration.
/// Shared among multiple ssl::stream instances.
/// Uses wolfSSL as the TLS backend.
///
/// Supported methods:
///   "tls_server"  — TLS server
///   "tls_client"  — TLS client
///   "tls"         — TLS (auto-detect)
///   "dtls_server" — DTLS server (for UDP)
///   "dtls_client" — DTLS client (for UDP)
///   "dtls"        — DTLS (auto-detect)
class context {
public:
    /// Create an SSL context for the given method.
    /// For server: use method = "tls_server"
    /// For client: use method = "tls_client"
    explicit context(const char* method = "tls");
    ~context();

    context(context&& other) noexcept;
    context& operator=(context&& other) noexcept;

    context(const context&) = delete;
    context& operator=(const context&) = delete;

    /// Load a certificate file (PEM format)
    bool use_certificate_file(const char* path);

    /// Load a private key file (PEM format)
    bool use_private_key_file(const char* path);

    /// Load CA certificates for verifying peer
    bool load_verify_file(const char* path);

    /// Set cipher list (e.g., "HIGH:!aNULL:!MD5")
    void set_cipher_list(const char* ciphers);

    /// Enable/disable peer verification
    void set_verify_peer(bool verify);

    /// Set ALPN protocols (client side): e.g., {"h2", "http/1.1"}
    void set_alpn_protos(const std::vector<std::string>& protos);

    /// Set ALPN select callback (server side): returns selected protocol
    void set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)> cb);

    /// Get the underlying WOLFSSL_CTX
    WOLFSSL_CTX* native_handle() { return ctx_; }

private:
    WOLFSSL_CTX* ctx_ = nullptr;
};

/// SSL/TLS stream — wraps a TCP socket with SSL encryption.
/// Provides async handshake, read, write, and shutdown via coroutines.
/// Uses wolfSSL as the TLS backend.
class stream {
public:
    /// Create an SSL stream over an existing TCP socket
    stream(tcp::socket& sock, context& ctx, bool is_server = false);
    ~stream();

    stream(stream&& other) noexcept;
    stream& operator=(stream&& other) noexcept;

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /// Async SSL handshake (client or server side)
    Task<int> async_handshake();

    /// Async SSL read
    Task<ssize_t> async_read_some(mutable_buffer buf);

    /// Async SSL write
    Task<ssize_t> async_write_some(const_buffer buf);

    /// Async SSL shutdown
    Task<int> async_shutdown();

    /// Get the selected ALPN protocol after handshake
    std::string alpn_selected() const;

    /// Get the underlying TCP socket
    tcp::socket& next_layer() { return *sock_; }
    const tcp::socket& next_layer() const { return *sock_; }

    /// Get the underlying WOLFSSL object
    WOLFSSL* native_handle() { return ssl_; }

private:
    WOLFSSL* ssl_ = nullptr;
    tcp::socket* sock_ = nullptr;
    bool is_server_ = false;
};

} // namespace ssl
} // namespace async_net
