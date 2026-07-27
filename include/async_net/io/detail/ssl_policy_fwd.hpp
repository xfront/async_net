#pragma once

// SSL policy forward declarations — policy-based design.
//
// This header defines the WolfSslPolicy and OpenSslPolicy structs with their
// type aliases and static method declarations. It uses forward declarations
// of the underlying C struct types (WOLFSSL_CTX, SSL, etc.) so that no SSL
// library headers are needed at the point of use.
//
// The actual method implementations are in:
//   src/security/ssl_wolfssl.cpp
//   src/security/ssl_openssl.cpp

#include <string>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
// Forward declarations of C struct types used by SSL libraries.
// These are compatible with the full type definitions that appear when the
// actual SSL library headers are included in the implementation .cpp files.
//   wolfSSL : WOLFSSL_CTX / WOLFSSL
//   OpenSSL : ssl_ctx_st / ssl_st    (typedef'd as SSL_CTX / SSL)
// ---------------------------------------------------------------------------

struct WOLFSSL_CTX;
struct WOLFSSL;
struct ssl_ctx_st;
struct ssl_st;

namespace async_net::ssl {

// ============================================================================
// WolfSslPolicy — static methods wrapping wolfSSL API
// ============================================================================

struct WolfSslPolicy {
    using ctx_type = WOLFSSL_CTX;
    using ssl_type = WOLFSSL;

    static void init();
    static void drain_errors(const char* prefix);

    static ctx_type* ctx_new(const char* method);
    static void ctx_free(ctx_type* ctx);
    static bool ctx_use_cert(ctx_type* ctx, const char* path);
    static bool ctx_use_key(ctx_type* ctx, const char* path);
    static bool ctx_load_verify(ctx_type* ctx, const char* path);
    static void ctx_set_cipher_list(ctx_type* ctx, const char* ciphers);
    static void ctx_set_verify(ctx_type* ctx, bool verify);
    static void ctx_set_alpn_protos(ctx_type* ctx, const unsigned char* wire, unsigned int len);
    static void ctx_set_alpn_select_cb(ctx_type* ctx,
        std::function<std::string(const std::vector<std::string>&)>* user_cb);

    static ssl_type* stream_new(ctx_type* ctx, int fd);
    static void stream_free(ssl_type* ssl);
    static void stream_set_accept_state(ssl_type* ssl);
    static void stream_set_connect_state(ssl_type* ssl);
    static int stream_do_handshake(ssl_type* ssl);
    static int stream_read(ssl_type* ssl, void* buf, int len);
    static int stream_write(ssl_type* ssl, const void* buf, int len);
    static int stream_shutdown(ssl_type* ssl);
    static int stream_get_error(ssl_type* ssl, int ret);
    static std::string stream_alpn_selected(ssl_type* ssl);
};

// ============================================================================
// OpenSslPolicy — static methods wrapping OpenSSL-compatible API
// ============================================================================

struct OpenSslPolicy {
    using ctx_type = ssl_ctx_st;
    using ssl_type = ssl_st;

    static void init();
    static void drain_errors(const char* prefix);

    static ctx_type* ctx_new(const char* method);
    static void ctx_free(ctx_type* ctx);
    static bool ctx_use_cert(ctx_type* ctx, const char* path);
    static bool ctx_use_key(ctx_type* ctx, const char* path);
    static bool ctx_load_verify(ctx_type* ctx, const char* path);
    static void ctx_set_cipher_list(ctx_type* ctx, const char* ciphers);
    static void ctx_set_verify(ctx_type* ctx, bool verify);
    static void ctx_set_alpn_protos(ctx_type* ctx, const unsigned char* wire, unsigned int len);
    static void ctx_set_alpn_select_cb(ctx_type* ctx,
        std::function<std::string(const std::vector<std::string>&)>* user_cb);

    static ssl_type* stream_new(ctx_type* ctx, int fd);
    static void stream_free(ssl_type* ssl);
    static void stream_set_accept_state(ssl_type* ssl);
    static void stream_set_connect_state(ssl_type* ssl);
    static int stream_do_handshake(ssl_type* ssl);
    static int stream_read(ssl_type* ssl, void* buf, int len);
    static int stream_write(ssl_type* ssl, const void* buf, int len);
    static int stream_shutdown(ssl_type* ssl);
    static int stream_get_error(ssl_type* ssl, int ret);
    static std::string stream_alpn_selected(ssl_type* ssl);
};

// ============================================================================
// Default SSL policy selection
// ============================================================================
// Exactly one of ASYNC_NET_SSL_WOLFSSL / ASYNC_NET_SSL_AWSLC /
// ASYNC_NET_SSL_LIBRESSL is defined by CMake when SSL is enabled.
// If none is defined, default_ssl_policy is NOT defined — the calling code
// (ssl.hpp) uses #ifdef ASYNC_NET_HAS_SSL to provide stub classes.

#if defined(ASYNC_NET_SSL_WOLFSSL)
    using default_ssl_policy = WolfSslPolicy;
#elif defined(ASYNC_NET_SSL_AWSLC) || defined(ASYNC_NET_SSL_LIBRESSL)
    using default_ssl_policy = OpenSslPolicy;
#endif

} // namespace async_net::ssl
