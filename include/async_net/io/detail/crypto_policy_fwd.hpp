#pragma once

// Crypto policy forward declarations — policy-based design.
//
// Defines WolfSslCryptoPolicy and OpenSslCryptoPolicy structs with static methods.
// No C struct forward declarations needed — crypto uses only standard types.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace async_net::crypto {

// ============================================================================
// WolfSslCryptoPolicy — static methods wrapping wolfSSL crypto API
// ============================================================================

struct WolfSslCryptoPolicy {
    static void init();

    static std::vector<uint8_t> aes_gcm_encrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* plaintext, size_t len,
        const uint8_t* aad, size_t aad_len);

    static std::optional<std::vector<uint8_t>> aes_gcm_decrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* ciphertext, size_t len,
        const uint8_t* aad, size_t aad_len);

    static std::vector<uint8_t> random_bytes(size_t len);
};

// ============================================================================
// OpenSslCryptoPolicy — static methods wrapping OpenSSL-compatible crypto API
// ============================================================================

struct OpenSslCryptoPolicy {
    static void init();

    static std::vector<uint8_t> aes_gcm_encrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* plaintext, size_t len,
        const uint8_t* aad, size_t aad_len);

    static std::optional<std::vector<uint8_t>> aes_gcm_decrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* ciphertext, size_t len,
        const uint8_t* aad, size_t aad_len);

    static std::vector<uint8_t> random_bytes(size_t len);
};

// ============================================================================
// Default crypto policy selection
// ============================================================================

#if defined(ASYNC_NET_SSL_WOLFSSL)
    using default_crypto_policy = WolfSslCryptoPolicy;
#elif defined(ASYNC_NET_SSL_AWSLC) || defined(ASYNC_NET_SSL_LIBRESSL)
    using default_crypto_policy = OpenSslCryptoPolicy;
#endif

} // namespace async_net::crypto
