#pragma once

#include "../detail/config.hpp"
#include "tcp.hpp"
#include "../coroutine/task.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

// ============================================================================
// Error code constants (policy-agnostic)
// ============================================================================

namespace async_net::ssl {

constexpr int ERR_NONE        = 0;
constexpr int ERR_WANT_READ   = 1;
constexpr int ERR_WANT_WRITE  = 2;
constexpr int ERR_ZERO_RETURN = 3;

} // namespace async_net::ssl

// ============================================================================
// When SSL is enabled: policy-based template classes
// When SSL is disabled: simple stub (non-template) classes
// ============================================================================

#ifdef ASYNC_NET_HAS_SSL

#include "detail/ssl_policy_fwd.hpp"

namespace async_net::ssl {

// ============================================================================
// basic_context<P> — SSL/TLS context templated on backend policy
// ============================================================================
//
// Supported methods:
//   "tls_server"  — TLS server
//   "tls_client"  — TLS client
//   "tls"         — TLS (auto-detect)
//   "dtls_server" — DTLS server (for UDP)
//   "dtls_client" — DTLS client (for UDP)
//   "dtls"        — DTLS (auto-detect)

template<typename P = default_ssl_policy>
class basic_context {
public:
    explicit basic_context(const char* method = "tls");
    ~basic_context();

    basic_context(basic_context&& other) noexcept;
    basic_context& operator=(basic_context&& other) noexcept;

    basic_context(const basic_context&) = delete;
    basic_context& operator=(const basic_context&) = delete;

    bool use_certificate_file(const char* path);
    bool use_private_key_file(const char* path);
    bool load_verify_file(const char* path);
    void set_cipher_list(const char* ciphers);
    void set_verify_peer(bool verify);
    void set_alpn_protos(const std::vector<std::string>& protos);
    void set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)> cb);

    // Get the underlying native context handle
    typename P::ctx_type* native_handle() { return ctx_; }

private:
    typename P::ctx_type* ctx_ = nullptr;
};

using context = basic_context<>;

// ============================================================================
// basic_stream<P> — SSL/TLS stream templated on backend policy
// ============================================================================

template<typename P = default_ssl_policy>
class basic_stream {
public:
    basic_stream(tcp::socket& sock, basic_context<P>& ctx, bool is_server = false);
    ~basic_stream();

    basic_stream(basic_stream&& other) noexcept;
    basic_stream& operator=(basic_stream&& other) noexcept;

    basic_stream(const basic_stream&) = delete;
    basic_stream& operator=(const basic_stream&) = delete;

    Task<int> async_handshake();
    Task<ssize_t> async_read_some(mutable_buffer buf);
    Task<ssize_t> async_write_some(const_buffer buf);
    Task<int> async_shutdown();

    std::string alpn_selected() const;

    tcp::socket& next_layer() { return *sock_; }
    const tcp::socket& next_layer() const { return *sock_; }

    // Get the underlying native SSL handle
    typename P::ssl_type* native_handle() { return ssl_; }

private:
    typename P::ssl_type* ssl_ = nullptr;
    tcp::socket* sock_ = nullptr;
    bool is_server_ = false;
};

using stream = basic_stream<>;

} // namespace async_net::ssl

#else

// Stub — no SSL
namespace async_net::ssl {

class context {
public:
    explicit context(const char* method = "tls") { (void)method; }
    ~context() = default;

    context(context&&) noexcept = default;
    context& operator=(context&&) noexcept = default;

    context(const context&) = delete;
    context& operator=(const context&) = delete;

    bool use_certificate_file(const char*) { return false; }
    bool use_private_key_file(const char*) { return false; }
    bool load_verify_file(const char*) { return false; }
    void set_cipher_list(const char*) {}
    void set_verify_peer(bool) {}
    void set_alpn_protos(const std::vector<std::string>&) {}
    void set_alpn_select_cb(std::function<std::string(const std::vector<std::string>&)>) {}

    void* native_handle() { return nullptr; }
};

class stream {
public:
    stream(tcp::socket& sock, context& ctx, bool is_server = false)
        : sock_(&sock) { (void)ctx; (void)is_server; }
    ~stream() = default;

    stream(stream&&) noexcept = default;
    stream& operator=(stream&&) noexcept = default;

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    Task<int> async_handshake() { co_return -1; }
    Task<ssize_t> async_read_some(mutable_buffer) { co_return -1; }
    Task<ssize_t> async_write_some(const_buffer) { co_return -1; }
    Task<int> async_shutdown() { co_return -1; }

    std::string alpn_selected() const { return {}; }

    tcp::socket& next_layer() { return *sock_; }
    const tcp::socket& next_layer() const { return *sock_; }

    void* native_handle() { return nullptr; }

private:
    tcp::socket* sock_ = nullptr;
};

} // namespace async_net::ssl

#endif // ASYNC_NET_HAS_SSL
