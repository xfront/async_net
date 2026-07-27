#include <async_net/crypto/aes_gcm.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include "crypto_backend.hpp"

namespace async_net::crypto {

// ---------------------------------------------------------------------------
// AES-256-GCM encrypt
// ---------------------------------------------------------------------------

std::vector<uint8_t> aes_gcm::encrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* plaintext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (key_len != KEY_LEN || iv_len != IV_LEN) return {};
    default_crypto_policy::init();
    return default_crypto_policy::aes_gcm_encrypt(key, key_len, iv, iv_len, plaintext, len, aad, aad_len);
}

// ---------------------------------------------------------------------------
// AES-256-GCM decrypt
// ---------------------------------------------------------------------------

std::optional<std::vector<uint8_t>> aes_gcm::decrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* ciphertext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (key_len != KEY_LEN || iv_len != IV_LEN) return std::nullopt;
    if (len < TAG_LEN) return std::nullopt;
    default_crypto_policy::init();
    return default_crypto_policy::aes_gcm_decrypt(key, key_len, iv, iv_len, ciphertext, len, aad, aad_len);
}

// ---------------------------------------------------------------------------
// Random bytes
// ---------------------------------------------------------------------------

std::vector<uint8_t> aes_gcm::random_bytes(size_t len) {
    default_crypto_policy::init();
    return default_crypto_policy::random_bytes(len);
}

} // namespace async_net::crypto

#else

// Stub implementation when SSL is not available
namespace async_net::crypto {

std::vector<uint8_t> aes_gcm::encrypt(
    const uint8_t*, size_t, const uint8_t*, size_t,
    const uint8_t*, size_t, const uint8_t*, size_t) { return {}; }

std::optional<std::vector<uint8_t>> aes_gcm::decrypt(
    const uint8_t*, size_t, const uint8_t*, size_t,
    const uint8_t*, size_t, const uint8_t*, size_t) { return std::nullopt; }

std::vector<uint8_t> aes_gcm::random_bytes(size_t) { return {}; }

} // namespace async_net::crypto

#endif // ASYNC_NET_HAS_SSL
