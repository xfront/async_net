#include <async_net/crypto/aes_gcm.hpp>

#ifdef ASYNC_NET_HAS_SSL

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

namespace async_net {
namespace crypto {

// Lazy-init wolfSSL (needed for RNG)
static void ensure_ssl_init() {
    static bool init = false;
    if (!init) {
        wolfSSL_Init();
        init = true;
    }
}

std::vector<uint8_t> aes_gcm::encrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* plaintext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (key_len != KEY_LEN || iv_len != IV_LEN) return {};

    ensure_ssl_init();

    // Output: ciphertext (same length as plaintext) + TAG_LEN
    std::vector<uint8_t> out(len + TAG_LEN);

    Aes aes;
    if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) return {};

    int ret = wc_AesGcmSetKey(&aes, key, static_cast<unsigned int>(key_len));
    if (ret != 0) { wc_AesFree(&aes); return {}; }

    // Encrypt into output buffer, tag goes to the end
    ret = wc_AesGcmEncrypt(&aes,
        out.data(),                          // output ciphertext
        plaintext, static_cast<unsigned int>(len),
        iv, static_cast<unsigned int>(iv_len),
        out.data() + len,                    // tag at end
        static_cast<unsigned int>(TAG_LEN),
        aad, static_cast<unsigned int>(aad_len));

    wc_AesFree(&aes);

    if (ret != 0) return {};
    return out;
}

std::optional<std::vector<uint8_t>> aes_gcm::decrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* ciphertext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (key_len != KEY_LEN || iv_len != IV_LEN) return std::nullopt;
    if (len < TAG_LEN) return std::nullopt;

    ensure_ssl_init();

    size_t ct_len = len - TAG_LEN;  // actual ciphertext length (without tag)
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
        tag, static_cast<unsigned int>(TAG_LEN),
        aad, static_cast<unsigned int>(aad_len));

    wc_AesFree(&aes);

    if (ret != 0) return std::nullopt;  // authentication failure
    return out;
}

std::vector<uint8_t> aes_gcm::random_bytes(size_t len) {
    ensure_ssl_init();

    std::vector<uint8_t> buf(len);
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0) return {};
    wc_RNG_GenerateBlock(&rng, buf.data(), static_cast<unsigned int>(len));
    wc_FreeRng(&rng);
    return buf;
}

} // namespace crypto
} // namespace async_net

#else

// Stub implementation when SSL is not available
namespace async_net {
namespace crypto {

std::vector<uint8_t> aes_gcm::encrypt(
    const uint8_t*, size_t, const uint8_t*, size_t,
    const uint8_t*, size_t, const uint8_t*, size_t) { return {}; }

std::optional<std::vector<uint8_t>> aes_gcm::decrypt(
    const uint8_t*, size_t, const uint8_t*, size_t,
    const uint8_t*, size_t, const uint8_t*, size_t) { return std::nullopt; }

std::vector<uint8_t> aes_gcm::random_bytes(size_t) { return {}; }

} // namespace crypto
} // namespace async_net

#endif // ASYNC_NET_HAS_SSL
