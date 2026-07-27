// wolfSSL crypto backend policy — replaces free functions with WolfSslCryptoPolicy static methods

#include "crypto_backend.hpp"

#ifdef ASYNC_NET_SSL_WOLFSSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

namespace async_net::crypto {

// ============================================================================
// WolfSslCryptoPolicy — static methods wrapping wolfSSL crypto API
// ============================================================================

void WolfSslCryptoPolicy::init() {
    static bool done = false;
    if (!done) { wolfSSL_Init(); done = true; }
}

std::vector<uint8_t> WolfSslCryptoPolicy::aes_gcm_encrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* plaintext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    std::vector<uint8_t> out(len + 16); // ciphertext + 16-byte tag

    Aes aes;
    if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) return {};

    int ret = wc_AesGcmSetKey(&aes, key, static_cast<unsigned int>(key_len));
    if (ret != 0) { wc_AesFree(&aes); return {}; }

    ret = wc_AesGcmEncrypt(&aes,
        out.data(),                          // output ciphertext
        plaintext, static_cast<unsigned int>(len),
        iv, static_cast<unsigned int>(iv_len),
        out.data() + len,                    // tag at end
        16,                                  // TAG_LEN
        aad, static_cast<unsigned int>(aad_len));

    wc_AesFree(&aes);
    if (ret != 0) return {};

    return out;
}

std::optional<std::vector<uint8_t>> WolfSslCryptoPolicy::aes_gcm_decrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* ciphertext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (len < 16) return std::nullopt;

    size_t ct_len = len - 16; // TAG_LEN
    const uint8_t* tag = ciphertext + ct_len;

    std::vector<uint8_t> out(ct_len);

    Aes aes;
    if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) return std::nullopt;

    int ret = wc_AesGcmSetKey(&aes, key, static_cast<unsigned int>(key_len));
    if (ret != 0) { wc_AesFree(&aes); return std::nullopt; }

    ret = wc_AesGcmDecrypt(&aes,
        out.data(),
        ciphertext, static_cast<unsigned int>(ct_len),
        iv, static_cast<unsigned int>(iv_len),
        tag, 16, // TAG_LEN
        aad, static_cast<unsigned int>(aad_len));

    wc_AesFree(&aes);
    if (ret != 0) return std::nullopt;

    return out;
}

std::vector<uint8_t> WolfSslCryptoPolicy::random_bytes(size_t len) {
    std::vector<uint8_t> buf(len);
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) return {};
    wc_RNG_GenerateBlock(&rng, buf.data(), static_cast<unsigned int>(len));
    wc_FreeRng(&rng);
    return buf;
}

} // namespace async_net::crypto

#endif // ASYNC_NET_SSL_WOLFSSL
