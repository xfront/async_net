#pragma once

#include "../detail/config.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>

namespace async_net::crypto {

/// AES-256-GCM authenticated encryption.
/// Backend is selected at build time (wolfSSL / AWS-LC / LibreSSL).
///
/// Wire format: [ciphertext...][16-byte GCM tag]
/// The caller provides key (32 bytes), iv/nonce (12 bytes),
/// and optional AAD (additional authenticated data).
class aes_gcm {
public:
    static constexpr size_t KEY_LEN = 32;   // AES-256
    static constexpr size_t IV_LEN = 12;    // GCM standard nonce size
    static constexpr size_t TAG_LEN = 16;   // GCM authentication tag

    /// Encrypt plaintext with AES-256-GCM.
    /// Returns ciphertext + 16-byte auth tag appended.
    /// Returns empty vector on failure.
    static std::vector<uint8_t> encrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* plaintext, size_t len,
        const uint8_t* aad = nullptr, size_t aad_len = 0);

    /// Decrypt ciphertext (with appended GCM tag) with AES-256-GCM.
    /// Returns plaintext on success, std::nullopt on auth failure.
    static std::optional<std::vector<uint8_t>> decrypt(
        const uint8_t* key, size_t key_len,
        const uint8_t* iv, size_t iv_len,
        const uint8_t* ciphertext, size_t len,
        const uint8_t* aad = nullptr, size_t aad_len = 0);

    /// Generate random bytes (uses crypto backend RNG).
    static std::vector<uint8_t> random_bytes(size_t len);
};

} // namespace async_net::crypto
