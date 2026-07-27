// OpenSSL-compatible crypto backend policy (AWS-LC / LibreSSL)
// Replaces free functions with OpenSslCryptoPolicy static methods.

#include "crypto_backend.hpp"

#if defined(ASYNC_NET_SSL_AWSLC) || defined(ASYNC_NET_SSL_LIBRESSL)

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

namespace async_net::crypto {

// ============================================================================
// OpenSslCryptoPolicy — static methods wrapping OpenSSL-compatible crypto API
// ============================================================================

void OpenSslCryptoPolicy::init() {
    static bool done = false;
    if (!done) { OPENSSL_init_ssl(0, nullptr); done = true; }
}

std::vector<uint8_t> OpenSslCryptoPolicy::aes_gcm_encrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* plaintext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    std::vector<uint8_t> out(len + 16); // ciphertext + 16-byte tag

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv_len), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }

    // AAD
    if (aad && aad_len > 0) {
        int aad_out_len = 0;
        if (EVP_EncryptUpdate(ctx, nullptr, &aad_out_len, aad, static_cast<int>(aad_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx); return {};
        }
    }

    // Encrypt
    int out_len = 0;
    if (EVP_EncryptUpdate(ctx, out.data(), &out_len, plaintext, static_cast<int>(len)) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }

    // Finalize
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, out.data() + out_len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }

    // Get tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + len) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }

    EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::optional<std::vector<uint8_t>> OpenSslCryptoPolicy::aes_gcm_decrypt(
    const uint8_t* key, size_t key_len,
    const uint8_t* iv, size_t iv_len,
    const uint8_t* ciphertext, size_t len,
    const uint8_t* aad, size_t aad_len)
{
    if (len < 16) return std::nullopt;

    size_t ct_len = len - 16; // TAG_LEN
    const uint8_t* tag = ciphertext + ct_len;

    std::vector<uint8_t> out(ct_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv_len), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt;
    }
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt;
    }

    // AAD
    if (aad && aad_len > 0) {
        int aad_out_len = 0;
        if (EVP_DecryptUpdate(ctx, nullptr, &aad_out_len, aad, static_cast<int>(aad_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx); return std::nullopt;
        }
    }

    // Decrypt
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx, out.data(), &out_len, ciphertext, static_cast<int>(ct_len)) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt;
    }

    // Set expected tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                            const_cast<uint8_t*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt;
    }

    // Finalize — verifies the tag
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, out.data() + out_len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx); return std::nullopt; // authentication failure
    }

    EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::vector<uint8_t> OpenSslCryptoPolicy::random_bytes(size_t len) {
    std::vector<uint8_t> buf(len);
    if (RAND_bytes(buf.data(), static_cast<int>(len)) != 1) return {};
    return buf;
}

} // namespace async_net::crypto

#endif // ASYNC_NET_SSL_AWSLC || ASYNC_NET_SSL_LIBRESSL
