#pragma once

// Internal crypto backend interface.
// Each SSL backend provides an implementation of these functions.
// This file is NOT part of the public API.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace async_net::crypto::backend {

/// Initialize the crypto backend (idempotent).
void init();

/// AES-256-GCM encrypt.
/// Returns ciphertext + 16-byte GCM tag appended.
/// Returns empty vector on failure.
std::vector<uint8_t> aes_gcm_encrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* plaintext, size_t len,
    const uint8_t* aad, size_t aad_len);

/// AES-256-GCM decrypt.
/// ciphertext includes the appended GCM tag.
/// Returns plaintext on success, std::nullopt on auth failure.
std::optional<std::vector<uint8_t>> aes_gcm_decrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* ciphertext, size_t len,
    const uint8_t* aad, size_t aad_len);

/// Generate cryptographically secure random bytes.
std::vector<uint8_t> random_bytes(size_t len);

} // namespace async_net::crypto::backend


