// HTTP/3 Session implementation — wolfSSL + ngtcp2 + self-contained QPACK/H3
extern "C" {
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
}

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/quic.h>

#include <async_net/http/http3_session.hpp>
#include "h3_frame.hpp"
#include "qpack.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <map>
#include <random>
#include <ctime>
#include <netdb.h>

namespace async_net {
namespace http {

// ============================================================================
// Utility: generate random bytes
// ============================================================================
static void gen_random(uint8_t* buf, size_t len) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(dist(gen));
}

static uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ============================================================================
// ngtcp2 callbacks (C linkage)
// ============================================================================

static int cb_recv_crypto_data(ngtcp2_conn* conn, ngtcp2_encryption_level level,
                               uint64_t offset, const uint8_t* data, size_t datalen,
                               void* user_data);

static int cb_handshake_completed(ngtcp2_conn* conn, void* user_data);

static int cb_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t* data, size_t datalen,
                               void* user_data, void* stream_user_data);

static int cb_stream_open(ngtcp2_conn* conn, int64_t stream_id, void* user_data);

static int cb_stream_close(ngtcp2_conn* conn, uint32_t flags,
                           int64_t stream_id, uint64_t app_error_code,
                           void* user_data, void* stream_user_data);

static int cb_acked_stream_data_offset(ngtcp2_conn* conn, int64_t stream_id,
                                       uint64_t offset, size_t datalen,
                                       void* user_data, void* stream_user_data);

static int cb_extend_max_stream_data(ngtcp2_conn* conn, int64_t stream_id,
                                     uint64_t max_data, void* user_data,
                                     void* stream_user_data);

static int cb_extend_max_remote_streams_bidi(ngtcp2_conn* conn, uint64_t max_streams,
                                             void* user_data);

static void cb_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* /*ctx*/) {
    gen_random(dest, destlen);
}

static int cb_get_new_connection_id(ngtcp2_conn* conn, ngtcp2_cid* cid,
                                    uint8_t* token, size_t cidlen, void* /*user_data*/) {
    gen_random(cid->data, cidlen);
    cid->datalen = cidlen;
    gen_random(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

static int cb_remove_connection_id(ngtcp2_conn* conn, const ngtcp2_cid* cid,
                                   void* user_data) {
    return 0;
}

static int cb_path_validation(ngtcp2_conn* conn, uint32_t flags,
                              const ngtcp2_path* path,
                              const ngtcp2_path* fallback_path,
                              ngtcp2_path_validation_result res,
                              void* user_data) {
    return 0;
}

static int cb_stream_reset(ngtcp2_conn* conn, int64_t stream_id,
                           uint64_t final_size, uint64_t app_error_code,
                           void* user_data, void* stream_user_data) {
    return 0;
}

static int cb_stream_stop_sending(ngtcp2_conn* conn, int64_t stream_id,
                                  uint64_t app_error_code, void* user_data,
                                  void* stream_user_data) {
    return 0;
}

static int cb_update_key(ngtcp2_conn* conn, uint8_t* rx_secret, uint8_t* tx_secret,
                         ngtcp2_crypto_aead_ctx* rx_aead_ctx, uint8_t* rx_iv,
                         ngtcp2_crypto_aead_ctx* tx_aead_ctx, uint8_t* tx_iv,
                         const uint8_t* current_rx_secret,
                         const uint8_t* current_tx_secret, size_t secretlen,
                         void* user_data) {
    // Delegate to ngtcp2_crypto_update_key_cb
    return ngtcp2_crypto_update_key_cb(conn, rx_secret, tx_secret,
                                       rx_aead_ctx, rx_iv,
                                       tx_aead_ctx, tx_iv,
                                       current_rx_secret, current_tx_secret,
                                       secretlen, user_data);
}

static int cb_recv_tx_key(ngtcp2_conn* conn, ngtcp2_encryption_level level,
                          void* user_data) {
    return 0;
}

static int cb_recv_rx_key(ngtcp2_conn* conn, ngtcp2_encryption_level level,
                          void* user_data) {
    return 0;
}

// Decrypt callback — wolfSSL QUIC AEAD decrypt
static int custom_decrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
                              const ngtcp2_crypto_aead_ctx *aead_ctx,
                              const uint8_t *ciphertext, size_t ciphertextlen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *aad, size_t aadlen) {
    (void)aead; (void)noncelen;
    if (wolfSSL_quic_aead_decrypt(dest, static_cast<WOLFSSL_EVP_CIPHER_CTX*>(aead_ctx->native_handle),
                                   ciphertext, ciphertextlen,
                                   nonce, aad, aadlen) != WOLFSSL_SUCCESS) {
        return -1;
    }
    return 0;
}

// Encrypt callback — wolfSSL QUIC AEAD encrypt
static int custom_encrypt_cb(uint8_t *dest, const ngtcp2_crypto_aead *aead,
                              const ngtcp2_crypto_aead_ctx *aead_ctx,
                              const uint8_t *plaintext, size_t plaintextlen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *aad, size_t aadlen) {
    (void)aead; (void)noncelen;
    if (wolfSSL_quic_aead_encrypt(dest, static_cast<WOLFSSL_EVP_CIPHER_CTX*>(aead_ctx->native_handle),
                                   plaintext, plaintextlen,
                                   nonce, aad, aadlen) != WOLFSSL_SUCCESS) {
        return -1;
    }
    return 0;
}

// Custom QUIC method — intercepts set_encryption_secrets for debugging,
// and delegates to ngtcp2_crypto functions.
static ngtcp2_encryption_level wolfssl_to_ngtcp2_level(WOLFSSL_ENCRYPTION_LEVEL lvl) {
    switch (lvl) {
        case wolfssl_encryption_initial:     return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
        case wolfssl_encryption_early_data:  return NGTCP2_ENCRYPTION_LEVEL_0RTT;
        case wolfssl_encryption_handshake:   return NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE;
        case wolfssl_encryption_application: return NGTCP2_ENCRYPTION_LEVEL_1RTT;
        default: return NGTCP2_ENCRYPTION_LEVEL_1RTT;
    }
}

static int quic_set_encryption_secrets(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                                        const uint8_t* read_secret,
                                        const uint8_t* write_secret,
                                        size_t secret_len) {
    auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(wolfSSL_get_app_data(ssl));
    if (!ref) return 0;
    ngtcp2_conn* conn = ref->get_conn(ref);
    if (!conn) return 0;

    auto ngtcp2_level = wolfssl_to_ngtcp2_level(level);

    if (read_secret) {
        auto rv = ngtcp2_crypto_derive_and_install_rx_key(
                conn, nullptr, nullptr, nullptr,
                ngtcp2_level, read_secret, secret_len);
        if (rv != 0) return -1;
    }
    if (write_secret) {
        auto rv = ngtcp2_crypto_derive_and_install_tx_key(
                conn, nullptr, nullptr, nullptr,
                ngtcp2_level, write_secret, secret_len);
        if (rv != 0) return -1;
    }
    return 1; // success
}

static int quic_add_handshake_data(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                                    const uint8_t* data, size_t len) {
    auto* ref = static_cast<ngtcp2_crypto_conn_ref*>(wolfSSL_get_app_data(ssl));
    if (!ref) return 0;
    ngtcp2_conn* conn = ref->get_conn(ref);
    if (!conn) return 0;
    auto rv = ngtcp2_conn_submit_crypto_data(conn, wolfssl_to_ngtcp2_level(level), data, len);
    if (rv != 0) {
        std::cerr << "[QUIC] submit_crypto_data err=" << ngtcp2_strerror(rv) << std::endl;
        return 0;
    }
    return 1;
}

static int quic_flush_flight(WOLFSSL*) { return 1; }

static int quic_send_alert(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level, uint8_t alert) {
    (void)ssl; (void)level; (void)alert;
    return 1;
}

static const WOLFSSL_QUIC_METHOD g_quic_method = {
    quic_set_encryption_secrets,
    quic_add_handshake_data,
    quic_flush_flight,
    quic_send_alert
};

// ============================================================================
// Internal implementation (pimpl)
// ============================================================================

struct http3_session::impl {
    using mode = http3_session::mode;

    // Per-stream state
    struct stream_state {
        int64_t stream_id = 0;
        enum st { idle, open, closed };
        st state = idle;

        // For request streams (bidirectional)
        request req;
        std::string body;
        bool headers_complete = false;
        bool end_stream = false;

        // Frame reader for H3 frames on this stream
        h3::frame_reader reader;
        bool control_stream = false;
        bool qpack_encoder_stream = false;
        bool qpack_decoder_stream = false;

        // Outgoing data buffer (H3 frames to send)
        std::string send_buf;
        uint64_t send_offset = 0;

        // For client: track response data
        response resp;
        std::string resp_body;
        bool resp_headers_complete = false;

        std::shared_ptr<http3_session::response_promise> promise;
    };

    mode mode_;
    config config_;
    bool alive_ = true;
    bool handshake_done_ = false;

    ngtcp2_conn* conn_ = nullptr;
    WOLFSSL_CTX* ssl_ctx_ = nullptr;
    WOLFSSL* ssl_ = nullptr;
    ngtcp2_crypto_conn_ref conn_ref_{};

    std::vector<std::string> output_pkts_;  // Each entry = one UDP datagram

    h3::qpack_encoder qpack_encoder_;
    h3::qpack_decoder qpack_decoder_;

    std::map<int64_t, stream_state> streams_;
    int64_t control_stream_id_ = -1;
    int64_t qpack_encoder_stream_id_ = -1;
    int64_t qpack_decoder_stream_id_ = -1;
    int64_t next_stream_id_ = 0;  // Server uses even IDs for push, odd for client

    request_handler request_handler_;
    http3_session::push_provider push_provider_;
    http3_session::push_handler push_handler_;
    std::vector<std::shared_ptr<http3_session::response_promise>> completed_promises_;

    // Push support
    uint64_t next_push_id_ = 0;
    uint64_t max_push_id_sent_ = 0;  // Track MAX_PUSH_ID sent to client

    // For client: queue request until handshake completes
    request pending_request_;
    std::shared_ptr<http3_session::response_promise> pending_promise_;
    bool has_pending_request_ = false;

    // For unidirectional stream type buffering
    std::map<int64_t, std::string> uni_stream_buf_;

    impl(mode m, const config& cfg) : mode_(m), config_(cfg) {}

    ~impl() {
        if (conn_) {
            ngtcp2_conn_del(conn_);
            conn_ = nullptr;
        }
        if (ssl_) {
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ssl_ctx_) {
            wolfSSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    int init_server(const uint8_t* dcid_data, size_t dcid_len,
                    const uint8_t* scid_data, size_t scid_len,
                    const ::sockaddr* local_addr, ::socklen_t local_addrlen,
                    const ::sockaddr* remote_addr, ::socklen_t remote_addrlen,
                    uint32_t version);

    int init_client(const char* host, uint16_t port,
                    const uint8_t* scid_data, size_t scid_len,
                    const ::sockaddr* local_addr, ::socklen_t local_addrlen,
                    const ::sockaddr* remote_addr, ::socklen_t remote_addrlen,
                    uint32_t version);

    // Open unidirectional control streams
    void open_control_streams();

    // Write HTTP/3 response on a bidirectional stream
    void submit_response(int64_t stream_id, const response& resp);

    // Process a complete H3 HEADERS frame on a request stream
    void handle_request_headers(int64_t stream_id, const std::string& payload);

    // Process a complete H3 DATA frame
    void handle_request_data(int64_t stream_id, const std::string& payload);

    // Process a complete H3 HEADERS frame on a response stream (client)
    void handle_response_headers(int64_t stream_id, const std::string& payload);

    // Process a complete H3 DATA frame on a response stream (client)
    void handle_response_data(int64_t stream_id, const std::string& payload);

    // Process SETTINGS frame on control stream
    void handle_settings(const std::string& payload);

    // Dispatch request to handler
    void dispatch_request(int64_t stream_id, stream_state& ss);

    // Submit a server push. Returns push_id or -1.
    int64_t do_submit_push(const request& promised_req, const response& push_resp,
                           int64_t associated_stream_id);

    // Handle PUSH_PROMISE frame (client)
    void handle_push_promise(int64_t stream_id, const std::string& payload);

    // Handle push stream data (client, stream type 0x01)
    void handle_push_stream(int64_t stream_id, const uint8_t* data, size_t datalen, bool fin);

    // Encode and send H3 frames
    std::string build_h3_headers(const std::vector<std::pair<std::string, std::string>>& hdrs);
    std::string build_h3_data(const std::string& body_data);

    // Write QUIC packets
    ssize_t write_packets();

    // Submit a queued request (called after handshake completes)
    void submit_queued_request();
};

// ============================================================================
// Callback implementations
// ============================================================================

static int cb_recv_crypto_data(ngtcp2_conn* conn, ngtcp2_encryption_level level,
                               uint64_t offset, const uint8_t* data, size_t datalen,
                               void* user_data) {
    auto rv = ngtcp2_crypto_read_write_crypto_data(conn, level, data, datalen);
    return rv;
}

static int cb_handshake_completed(ngtcp2_conn* conn, void* user_data) {
    auto* self = static_cast<http3_session::impl*>(user_data);
    self->handshake_done_ = true;
    self->open_control_streams();

    // Client: submit queued request after handshake
    if (self->mode_ == http3_session::impl::mode::client && self->has_pending_request_) {
        self->submit_queued_request();
    }
    return 0;
}

static int cb_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t* data, size_t datalen,
                               void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<http3_session::impl*>(user_data);
    bool fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;

    // Unidirectional stream (bit 1 of stream ID = 1)
    if ((stream_id & 0x02) != 0) {
        auto& ss = self->streams_[stream_id];
        ss.stream_id = stream_id;

        // Check if we already identified this stream type
        auto& buf = self->uni_stream_buf_[stream_id];
        buf.append(reinterpret_cast<const char*>(data), datalen);

        if (buf.size() < 1) {
            // Need stream type byte
            if (fin) ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
            return 0;
        }

        // Decode stream type varint
        uint64_t stream_type;
        int consumed = h3::decode_varint(
            reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), stream_type);
        if (consumed <= 0) {
            // Need more data for varint
            if (fin) ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
            return 0;
        }

        // Process remaining data based on stream type
        const char* payload = buf.data() + consumed;
        size_t payload_len = buf.size() - consumed;

        if (stream_type == static_cast<uint64_t>(h3::stream_type::CONTROL)) {
            ss.control_stream = true;
            // Parse H3 frames from control stream
            h3::frame_reader reader;
            size_t fed = reader.feed(reinterpret_cast<const uint8_t*>(payload), payload_len);
            while (reader.has_frame()) {
                auto f = reader.take_frame();
                if (f.type == h3::frame_type::SETTINGS) {
                    self->handle_settings(f.payload);
                } else if (f.type == h3::frame_type::PUSH_PROMISE) {
                    self->handle_push_promise(stream_id, f.payload);
                }
                fed += reader.feed(reinterpret_cast<const uint8_t*>(payload + fed), payload_len - fed);
            }
        } else if (stream_type == static_cast<uint64_t>(h3::stream_type::PUSH)) {
            // Server push stream (client only)
            self->handle_push_stream(stream_id,
                reinterpret_cast<const uint8_t*>(payload), payload_len, fin);
        } else if (stream_type == static_cast<uint64_t>(h3::stream_type::QPACK_ENCODER)) {
            ss.qpack_encoder_stream = true;
            self->qpack_decoder_.process_encoder_instructions(
                reinterpret_cast<const uint8_t*>(payload), payload_len);
        } else if (stream_type == static_cast<uint64_t>(h3::stream_type::QPACK_DECODER)) {
            ss.qpack_decoder_stream = true;
            self->qpack_decoder_.process_decoder_instructions(
                reinterpret_cast<const uint8_t*>(payload), payload_len);
        }

        ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
        return 0;
    }

    // Bidirectional stream (request/response)
    auto& ss = self->streams_[stream_id];
    ss.stream_id = stream_id;
    if (ss.state == http3_session::impl::stream_state::idle) {
        ss.state = http3_session::impl::stream_state::open;
    }

    // Feed H3 frame reader
    size_t fed = ss.reader.feed(data, datalen);
    while (ss.reader.has_frame()) {
        auto f = ss.reader.take_frame();
        if (f.type == h3::frame_type::HEADERS) {
            self->handle_request_headers(stream_id, f.payload);
        } else if (f.type == h3::frame_type::DATA) {
            self->handle_request_data(stream_id, f.payload);
        }
        // Feed any remaining bytes
        fed += ss.reader.feed(data + fed, datalen - fed);
    }

    if (fin) {
        ss.end_stream = true;
        if (self->mode_ == http3_session::impl::mode::server) {
            if (ss.headers_complete) {
                self->dispatch_request(stream_id, ss);
            }
        } else {
            // Client: complete the promise when FIN is received and headers are done
            if (ss.resp_headers_complete && ss.promise && !ss.promise->complete) {
                auto& p = ss.promise;
                p->resp = std::move(ss.resp);
                p->resp.bd = body(ss.resp_body);
                p->resp.ver = version::HTTP_3;
                p->complete = true;
                if (p->waiter) {
                    p->waiter.resume();
                }
            }
        }
    }

    // Extend stream offset to allow more data
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    return 0;
}

static int cb_stream_open(ngtcp2_conn* conn, int64_t stream_id, void* user_data) {
    auto* self = static_cast<http3_session::impl*>(user_data);
    self->streams_[stream_id].stream_id = stream_id;
    return 0;
}

static int cb_stream_close(ngtcp2_conn* conn, uint32_t flags,
                           int64_t stream_id, uint64_t app_error_code,
                           void* user_data, void* /*stream_user_data*/) {
    auto* self = static_cast<http3_session::impl*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        // Client: complete the response promise when stream closes
        if (self->mode_ == http3_session::impl::mode::client && it->second.promise) {
            if (!it->second.promise->complete) {
                auto& p = it->second.promise;
                if (it->second.resp_headers_complete) {
                    p->resp.bd = body(it->second.resp_body);
                    p->resp.ver = version::HTTP_3;
                    p->complete = true;
                } else {
                    p->error = true;
                    p->complete = true;
                }
                if (p->waiter) {
                    p->waiter.resume();
                }
            }
        }
        it->second.state = http3_session::impl::stream_state::closed;
    }
    return 0;
}

static int cb_acked_stream_data_offset(ngtcp2_conn* conn, int64_t stream_id,
                                       uint64_t offset, size_t datalen,
                                       void* user_data, void* /*stream_user_data*/) {
    return 0;
}

static int cb_extend_max_stream_data(ngtcp2_conn* conn, int64_t stream_id,
                                     uint64_t max_data, void* user_data,
                                     void* /*stream_user_data*/) {
    return 0;
}

static int cb_extend_max_remote_streams_bidi(ngtcp2_conn* conn, uint64_t max_streams,
                                             void* user_data) {
    return 0;
}

// ============================================================================
// impl methods
// ============================================================================

static void ngtcp2_log(void* user_data, char* log, size_t len) {
    (void)user_data; (void)log; (void)len;
    // ngtcp2 internal logging — disabled for production
}

int http3_session::impl::init_server(
    const uint8_t* dcid_data, size_t dcid_len,
    const uint8_t* scid_data, size_t scid_len,
    const ::sockaddr* local_addr, ::socklen_t local_addrlen,
    const ::sockaddr* remote_addr, ::socklen_t remote_addrlen,
    uint32_t version) {

    // Initialize wolfSSL
    wolfSSL_Init();

    // Create SSL context with QUIC support
    ssl_ctx_ = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (!ssl_ctx_) {
        std::cerr << "[H3] wolfSSL_CTX_new failed" << std::endl;
        return -1;
    }

    // Configure for TLS 1.3 (QUIC requires TLS 1.3)
    wolfSSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
    wolfSSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

    if (!config_.cert_file.empty()) {
        if (wolfSSL_CTX_use_certificate_chain_file(ssl_ctx_, config_.cert_file.c_str()) != 1) {
            std::cerr << "[H3] Failed to load certificate: " << config_.cert_file << std::endl;
            return -1;
        }
    }
    if (!config_.key_file.empty()) {
        if (wolfSSL_CTX_use_PrivateKey_file(ssl_ctx_, config_.key_file.c_str(),
                                             SSL_FILETYPE_PEM) != 1) {
            std::cerr << "[H3] Failed to load private key: " << config_.key_file << std::endl;
            return -1;
        }
    }

    // Set ALPN for h3
    wolfSSL_CTX_set_alpn_select_cb(ssl_ctx_,
        [](WOLFSSL* ssl, const unsigned char** out, unsigned char* outlen,
           const unsigned char* in, unsigned int inlen, void* arg) -> int {
            // Look for "h3" in ALPN
            for (unsigned int i = 0; i < inlen; ) {
                uint8_t len = in[i];
                if (i + 1 + len > inlen) break;
                if (len == 2 && memcmp(in + i + 1, "h3", 2) == 0) {
                    *out = in + i + 1;
                    *outlen = 2;
                    return 0;
                }
                i += 1 + len;
            }
            return 1; // No match
        }, nullptr);

    // Create SSL object
    ssl_ = wolfSSL_new(ssl_ctx_);
    if (!ssl_) {
        std::cerr << "[H3] wolfSSL_new failed" << std::endl;
        return -1;
    }

    // Set up ngtcp2 connection
    auto callbacks = ngtcp2_callbacks{};
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = cb_recv_crypto_data;
    callbacks.handshake_completed = cb_handshake_completed;
    callbacks.encrypt = custom_encrypt_cb;
    callbacks.decrypt = custom_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_stream_data = cb_recv_stream_data;
    callbacks.acked_stream_data_offset = cb_acked_stream_data_offset;
    callbacks.stream_open = cb_stream_open;
    callbacks.stream_close = cb_stream_close;
    callbacks.rand = cb_rand;
    callbacks.get_new_connection_id = cb_get_new_connection_id;
    callbacks.remove_connection_id = cb_remove_connection_id;
    callbacks.update_key = cb_update_key;
    callbacks.path_validation = cb_path_validation;
    callbacks.stream_reset = cb_stream_reset;
    callbacks.extend_max_remote_streams_bidi = cb_extend_max_remote_streams_bidi;
    callbacks.extend_max_stream_data = cb_extend_max_stream_data;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.recv_tx_key = cb_recv_tx_key;
    callbacks.recv_rx_key = cb_recv_rx_key;

    ngtcp2_cid dcid, scid;
    ngtcp2_cid_init(&dcid, dcid_data, dcid_len);  // client Initial DCID = server's own CID
    ngtcp2_cid_init(&scid, scid_data, scid_len);   // client Initial SCID = client's CID

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ms() * 1000000ULL; // nanoseconds
    settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
    settings.no_pmtud = 1;
    settings.max_tx_udp_payload_size = 1200;
    settings.log_write = ngtcp2_log;

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = config_.max_streams;
    params.initial_max_streams_uni = 3;
    params.max_idle_timeout = config_.idle_timeout_ms * 1000000ULL;
    params.active_connection_id_limit = 7;

    params.original_dcid_present = 1;
    ngtcp2_cid_init(&params.original_dcid, dcid_data, dcid_len);

    ngtcp2_path path;
    ngtcp2_addr local_naddr, remote_naddr;
    ngtcp2_addr_init(&local_naddr, local_addr, local_addrlen);
    ngtcp2_addr_init(&remote_naddr, remote_addr, remote_addrlen);
    path.local = local_naddr;
    path.remote = remote_naddr;
    path.user_data = nullptr;

    // ngtcp2_conn_server_new: dcid param = client's Initial SCID, scid param = server's own CID
    auto rv = ngtcp2_conn_server_new(&conn_, &scid, &dcid, &path, version,
                                     &callbacks, &settings, &params, nullptr, this);
    if (rv != 0) {
        return -1;
    }

    // Set TLS handle BEFORE setting transport params
    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);

    // Set conn_ref as app data so ngtcp2 QUIC method callbacks can find the conn
    conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
        auto* self = reinterpret_cast<impl*>(
            reinterpret_cast<char*>(ref) - offsetof(impl, conn_ref_));
        return self->conn_;
    };
    conn_ref_.user_data = this;
    wolfSSL_set_app_data(ssl_, &conn_ref_);

    // Install our custom QUIC method for detailed logging
    wolfSSL_set_quic_method(ssl_, &g_quic_method);

    // Set transport params on ngtcp2 conn
    ngtcp2_conn_set_local_transport_params(conn_, &params);

    return 0;
}

int http3_session::impl::init_client(
    const char* host, uint16_t port,
    const uint8_t* scid_data, size_t scid_len,
    const ::sockaddr* local_addr, ::socklen_t local_addrlen,
    const ::sockaddr* remote_addr, ::socklen_t remote_addrlen,
    uint32_t version) {

    wolfSSL_Init();

    // Create TLS 1.3 client context
    ssl_ctx_ = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ssl_ctx_) {
        std::cerr << "[H3] client: wolfSSL_CTX_new failed" << std::endl;
        return -1;
    }

    // Configure for TLS 1.3 (QUIC requires TLS 1.3)
    wolfSSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
    wolfSSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

    // Disable certificate verification (self-signed certs)
    wolfSSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

    // Set ALPN to "h3" (client side)
    const unsigned char alpn_h3[] = {2, 'h', '3'};
    wolfSSL_CTX_set_alpn_protos(ssl_ctx_, alpn_h3, sizeof(alpn_h3));

    // Create SSL object
    ssl_ = wolfSSL_new(ssl_ctx_);
    if (!ssl_) {
        std::cerr << "[H3] client: wolfSSL_new failed" << std::endl;
        return -1;
    }

    // Generate random SCID
    uint8_t scid_buf[NGTCP2_MAX_CIDLEN];
    gen_random(scid_buf, scid_len);
    ngtcp2_cid scid;
    ngtcp2_cid_init(&scid, scid_buf, scid_len);

    // Generate random DCID (must be >= 8 bytes per QUIC spec)
    uint8_t dcid_buf[8];
    gen_random(dcid_buf, sizeof(dcid_buf));
    ngtcp2_cid dcid;
    ngtcp2_cid_init(&dcid, dcid_buf, sizeof(dcid_buf));

    // Set up ngtcp2 callbacks
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = cb_recv_crypto_data;
    callbacks.handshake_completed = cb_handshake_completed;
    callbacks.encrypt = custom_encrypt_cb;
    callbacks.decrypt = custom_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_stream_data = cb_recv_stream_data;
    callbacks.acked_stream_data_offset = cb_acked_stream_data_offset;
    callbacks.stream_open = cb_stream_open;
    callbacks.stream_close = cb_stream_close;
    callbacks.rand = cb_rand;
    callbacks.get_new_connection_id = cb_get_new_connection_id;
    callbacks.remove_connection_id = cb_remove_connection_id;
    callbacks.update_key = cb_update_key;
    callbacks.path_validation = cb_path_validation;
    callbacks.stream_reset = cb_stream_reset;
    callbacks.extend_max_remote_streams_bidi = cb_extend_max_remote_streams_bidi;
    callbacks.extend_max_stream_data = cb_extend_max_stream_data;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.recv_tx_key = cb_recv_tx_key;
    callbacks.recv_rx_key = cb_recv_rx_key;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;

    // Settings
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ms() * 1000000ULL;
    settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
    settings.no_pmtud = 1;
    settings.max_tx_udp_payload_size = 1200;
    settings.log_write = ngtcp2_log;

    // Transport params
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = config_.max_streams;
    params.initial_max_streams_uni = 3;
    params.max_idle_timeout = config_.idle_timeout_ms * 1000000ULL;
    params.active_connection_id_limit = 7;

    // Path
    ngtcp2_path path;
    ngtcp2_addr local_naddr, remote_naddr;
    ngtcp2_addr_init(&local_naddr, local_addr, local_addrlen);
    ngtcp2_addr_init(&remote_naddr, remote_addr, remote_addrlen);
    path.local = local_naddr;
    path.remote = remote_naddr;
    path.user_data = nullptr;

    auto rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path, version,
                                     &callbacks, &settings, &params, nullptr, this);
    if (rv != 0) {
        std::cerr << "[H3] ngtcp2_conn_client_new: " << ngtcp2_strerror(rv) << std::endl;
        return -1;
    }

    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);

    // Set conn_ref as app data so ngtcp2 QUIC method callbacks can find the conn
    conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
        auto* self = reinterpret_cast<impl*>(
            reinterpret_cast<char*>(ref) - offsetof(impl, conn_ref_));
        return self->conn_;
    };
    conn_ref_.user_data = this;
    wolfSSL_set_app_data(ssl_, &conn_ref_);

    // Install our custom QUIC method
    wolfSSL_set_quic_method(ssl_, &g_quic_method);

    // Generate initial Client Hello packet
    write_packets();

    return 0;
}

void http3_session::impl::open_control_streams() {
    if (control_stream_id_ >= 0) return;

    // Open control stream (unidirectional)
    int64_t stream_id;
    auto rv = ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
    if (rv != 0) return;
    control_stream_id_ = stream_id;

    // Send stream type: CONTROL (0x00)
    std::string ctrl_data;
    h3::append_varint(ctrl_data, static_cast<uint64_t>(h3::stream_type::CONTROL));

    // Send SETTINGS frame
    std::vector<h3::setting_entry> settings = {
        {h3::setting_id::MAX_FIELD_SECTION_SIZE, 8192},
    };
    ctrl_data.append(h3::build_settings_frame(settings));

    // Write control stream data
    uint8_t pkt_buf[1472];
    ngtcp2_vec datavec;
    datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(ctrl_data.data()));
    datavec.len = ctrl_data.size();
    ngtcp2_ssize pdatalen = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        conn_, nullptr, nullptr,
        pkt_buf, sizeof(pkt_buf),
        &pdatalen,
        NGTCP2_WRITE_STREAM_FLAG_NONE,
        control_stream_id_, &datavec, 1,
        now_ms() * 1000000ULL);
    if (nwrite > 0) {
        output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
    }

    // Open QPACK encoder stream
    rv = ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
    if (rv == 0) {
        qpack_encoder_stream_id_ = stream_id;
        std::string enc_data;
        h3::append_varint(enc_data, static_cast<uint64_t>(h3::stream_type::QPACK_ENCODER));
        ngtcp2_vec enc_vec = {reinterpret_cast<uint8_t*>(const_cast<char*>(enc_data.data())),
                              enc_data.size()};
        ngtcp2_ssize enc_pdatalen = 0;
        auto enc_nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &enc_pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            qpack_encoder_stream_id_, &enc_vec, 1,
            now_ms() * 1000000ULL);
        if (enc_nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), enc_nwrite);
        }
    }

    // Open QPACK decoder stream
    rv = ngtcp2_conn_open_uni_stream(conn_, &stream_id, nullptr);
    if (rv == 0) {
        qpack_decoder_stream_id_ = stream_id;
        std::string dec_data;
        h3::append_varint(dec_data, static_cast<uint64_t>(h3::stream_type::QPACK_DECODER));
        ngtcp2_vec dec_vec = {reinterpret_cast<uint8_t*>(const_cast<char*>(dec_data.data())),
                              dec_data.size()};
        ngtcp2_ssize dec_pdatalen = 0;
        auto dec_nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &dec_pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            qpack_decoder_stream_id_, &dec_vec, 1,
            now_ms() * 1000000ULL);
        if (dec_nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), dec_nwrite);
        }
    }
}

void http3_session::impl::handle_request_headers(int64_t stream_id,
                                                  const std::string& payload) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;
    auto& ss = it->second;

    // Client mode: these are response headers from the server
    if (mode_ == mode::client) {
        handle_response_headers(stream_id, payload);
        return;
    }

    auto headers = qpack_decoder_.decode(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    ss.headers_complete = true;
    for (auto& [name, value] : headers) {
        if (name == ":method") {
            auto m = parse_method(value);
            if (m) ss.req.method = *m;
        } else if (name == ":path") {
            ss.req.path = value;
        } else if (name == ":authority") {
            ss.req.hdrs.append("Host", value);
        } else if (name == ":scheme") {
            // Ignore for now
        } else if (name[0] != ':') {
            ss.req.hdrs.append(name, value);
        }
    }
    ss.req.ver = version::HTTP_3;

    // If no body expected (e.g., GET), dispatch immediately
    if (ss.end_stream) {
        dispatch_request(stream_id, ss);
    }
}

void http3_session::impl::handle_request_data(int64_t stream_id,
                                               const std::string& payload) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;
    auto& ss = it->second;

    // Client mode: this is response body data
    if (mode_ == mode::client) {
        handle_response_data(stream_id, payload);
        return;
    }

    ss.body.append(payload);

    if (ss.end_stream) {
        ss.req.bd = body(ss.body);
        if (ss.headers_complete && mode_ == mode::server) {
            dispatch_request(stream_id, ss);
        }
    }
}

void http3_session::impl::handle_response_headers(int64_t stream_id,
                                                   const std::string& payload) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;
    auto& ss = it->second;

    auto headers = qpack_decoder_.decode(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    ss.resp_headers_complete = true;
    for (auto& [name, value] : headers) {
        if (name == ":status") {
            int code = std::atoi(value.c_str());
            ss.resp.status = status_code(code > 0 ? code : 500);
        } else if (name[0] != ':') {
            ss.resp.hdrs.set(name, value);
        }
    }
    ss.resp.ver = version::HTTP_3;

    // If FIN was already received, complete the promise now
    if (ss.end_stream && ss.promise && !ss.promise->complete) {
        auto& p = ss.promise;
        p->resp = std::move(ss.resp);
        p->resp.bd = body(ss.resp_body);
        p->complete = true;
        if (p->waiter) {
            p->waiter.resume();
        }
    }
}

void http3_session::impl::handle_response_data(int64_t stream_id,
                                                const std::string& payload) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;
    auto& ss = it->second;

    ss.resp_body.append(payload);

    // If FIN was received and headers are complete, complete the promise
    if (ss.end_stream && ss.resp_headers_complete && ss.promise && !ss.promise->complete) {
        auto& p = ss.promise;
        p->resp = std::move(ss.resp);
        p->resp.bd = body(ss.resp_body);
        p->complete = true;
        if (p->waiter) {
            p->waiter.resume();
        }
    }
}

void http3_session::impl::handle_settings(const std::string& payload) {
    // Parse and store peer settings (for now, just acknowledge)
    auto settings = h3::parse_settings(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    for (auto& s : settings) {
        // Store settings as needed
        (void)s;
    }
}

void http3_session::impl::dispatch_request(int64_t stream_id, stream_state& ss) {
    if (!request_handler_) return;

    ss.req.bd = body(ss.body);
    response resp = request_handler_(ss.req);

    submit_response(stream_id, resp);

    // Invoke push provider to send associated resources
    if (push_provider_) {
        auto pushes = push_provider_(ss.req);
        for (auto& [promised_req, push_resp] : pushes) {
            do_submit_push(promised_req, push_resp, stream_id);
        }
    }
}

void http3_session::impl::submit_response(int64_t stream_id, const response& resp) {
    // Build headers
    std::vector<std::pair<std::string, std::string>> hdrs;
    hdrs.push_back({":status", std::to_string(resp.status.as_int())});
    for (auto& [k, v] : resp.hdrs) {
        hdrs.push_back({k, v});
    }

    // Encode headers with QPACK
    std::string qpack_data = qpack_encoder_.encode_static(hdrs);

    // Build H3 frames
    std::string frames = h3::build_headers_frame(qpack_data);
    if (!resp.bd.empty()) {
        frames.append(h3::build_data_frame(resp.bd.data()));
    }

    auto& ss = streams_[stream_id];
    ss.send_buf.append(frames);

    // Write stream data via ngtcp2
    uint8_t pkt_buf[1472];
    while (ss.send_offset < ss.send_buf.size()) {
        size_t remaining = ss.send_buf.size() - ss.send_offset;
        ngtcp2_vec datavec;
        datavec.base = reinterpret_cast<uint8_t*>(
            const_cast<char*>(ss.send_buf.data() + ss.send_offset));
        datavec.len = remaining;

        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_FIN,
            stream_id, &datavec, 1,
            now_ms() * 1000000ULL);

        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                break;
            }
            if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                // Continue writing
                continue;
            }
            break;
        }

        if (nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
        }

        // The stream_data_written callback tells us how much was accepted
        // For simplicity, advance offset by what we sent
        // In real impl, we track this via ngtcp2 callbacks
        break; // Send one packet at a time
    }
}

std::string http3_session::impl::build_h3_headers(
    const std::vector<std::pair<std::string, std::string>>& hdrs) {
    std::string qpack_data = qpack_encoder_.encode_static(hdrs);
    return h3::build_headers_frame(qpack_data);
}

std::string http3_session::impl::build_h3_data(const std::string& body_data) {
    return h3::build_data_frame(body_data);
}

ssize_t http3_session::impl::write_packets() {
    uint8_t pkt_buf[1472];
    ssize_t total = 0;

    for (;;) {
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            -1, nullptr, 0,
            now_ms() * 1000000ULL);

        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_WRITE_MORE) continue;
            return -1;
        }
        if (nwrite == 0) break;

        output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
        total += nwrite;
    }

    return total;
}

// ============================================================================
// Public API implementation
// ============================================================================

http3_session::http3_session(mode m, const config& cfg)
    : impl_(std::make_unique<impl>(m, cfg)) {
    if (m == mode::client) {
        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = 0;

        struct sockaddr_in remote_addr{};
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(cfg.port);
        // Try numeric IP first
        if (::inet_pton(AF_INET, cfg.host.c_str(), &remote_addr.sin_addr) <= 0) {
            // Try DNS resolution
            struct addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo* res = nullptr;
            if (::getaddrinfo(cfg.host.c_str(), nullptr, &hints, &res) == 0 && res) {
                auto* sin = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr);
                remote_addr.sin_addr = sin->sin_addr;
                ::freeaddrinfo(res);
            }
        }

        uint8_t scid[8] = {};
        impl_->init_client(cfg.host.c_str(), cfg.port,
                          scid, sizeof(scid),
                          reinterpret_cast<const ::sockaddr*>(&local_addr), sizeof(local_addr),
                          reinterpret_cast<const ::sockaddr*>(&remote_addr), sizeof(remote_addr),
                          NGTCP2_PROTO_VER_V1);
    }
}

http3_session::http3_session(mode m)
    : impl_(std::make_unique<impl>(m, config{})) {}

http3_session::~http3_session() = default;
http3_session::http3_session(http3_session&&) noexcept = default;
http3_session& http3_session::operator=(http3_session&&) noexcept = default;

ssize_t http3_session::feed_packet(const uint8_t* data, size_t len,
                                    const ::sockaddr* local_addr, ::socklen_t local_addrlen,
                                    const ::sockaddr* remote_addr, ::socklen_t remote_addrlen) {
    if (!impl_ || !impl_->alive_ || !impl_->conn_) return -1;

    ngtcp2_path path;
    ngtcp2_pkt_info pi{};

    // Set path addresses if provided (needed for ngtcp2 path validation)
    if (local_addr && remote_addr) {
        ngtcp2_addr_init(&path.local, local_addr, local_addrlen);
        ngtcp2_addr_init(&path.remote, remote_addr, remote_addrlen);
    } else {
        memset(&path, 0, sizeof(path));
    }

    auto rv = ngtcp2_conn_read_pkt(impl_->conn_, &path, &pi, data, len,
                                   now_ms() * 1000000ULL);
    if (rv != 0) {
        std::cerr << "[H3 feed_pkt] err=" << ngtcp2_strerror(rv) << std::endl;
        impl_->alive_ = false;
        return -1;
    }

    // Generate any output packets
    impl_->write_packets();
    return static_cast<ssize_t>(len);
}

bool http3_session::init_server_from_packet(const uint8_t* data, size_t len,
                                             const ::sockaddr* local_addr, ::socklen_t local_addrlen,
                                             const ::sockaddr* remote_addr, ::socklen_t remote_addrlen) {
    if (!impl_ || impl_->conn_) return impl_ && impl_->conn_;
    if (impl_->mode_ != impl::mode::server) return false;
    if (len < 5) return false;

    // Parse QUIC Long Header to extract DCID and SCID
    // Byte 0: Header form(1) | Fixed(1) | Long type(2) | Reserved(2) | PN len(2)
    // Bytes 1-4: Version
    // Byte 5: DCID length
    // Bytes 6..6+dcid_len-1: DCID
    // Next byte: SCID length
    // Next scid_len bytes: SCID

    if ((data[0] & 0x80) == 0) return false; // Not a long header

    // Extract version (bytes 1-4)
    uint32_t version = (static_cast<uint32_t>(data[1]) << 24) |
                       (static_cast<uint32_t>(data[2]) << 16) |
                       (static_cast<uint32_t>(data[3]) << 8) |
                       static_cast<uint32_t>(data[4]);

    size_t offset = 5;
    if (offset >= len) return false;

    uint8_t dcid_len = data[offset++];
    if (offset + dcid_len > len || dcid_len > NGTCP2_MAX_CIDLEN) return false;
    const uint8_t* dcid_data = data + offset;
    offset += dcid_len;

    if (offset >= len) return false;
    uint8_t scid_len = data[offset++];
    if (offset + scid_len > len || scid_len > NGTCP2_MAX_CIDLEN) return false;
    const uint8_t* scid_data = data + offset;

    // - dcid (first arg) = client's Initial DCID → server uses as its SCID
    // - scid (second arg) = client's SCID → server uses to address peer

    int rv = impl_->init_server(dcid_data, dcid_len,
                                scid_data, scid_len,
                                local_addr, local_addrlen,
                                remote_addr, remote_addrlen,
                                version);
    if (rv != 0) std::cerr << "[H3] init_server failed" << std::endl;
    return rv == 0;
}

std::string http3_session::get_pending_output() {
    if (!impl_) return {};
    // Concatenate all packets into a single string (for backward compat)
    std::string out;
    for (auto& pkt : impl_->output_pkts_) out.append(pkt);
    impl_->output_pkts_.clear();
    return out;
}

std::vector<std::string> http3_session::get_pending_packets() {
    if (!impl_) return {};
    auto pkts = std::move(impl_->output_pkts_);
    impl_->output_pkts_.clear();
    return pkts;
}

bool http3_session::is_alive() const {
    return impl_ && impl_->alive_;
}

bool http3_session::handshake_complete() const {
    return impl_ && impl_->handshake_done_;
}

Task<bool> http3_session::flush(send_fn fn) {
    auto pkts = get_pending_packets();
    if (pkts.empty()) co_return true;

    for (auto& data : pkts) {
        if (data.empty()) continue;
        size_t sent = 0;
        while (sent < data.size()) {
            auto n = co_await fn(
                reinterpret_cast<const uint8_t*>(data.data() + sent),
                data.size() - sent);
            if (n <= 0) co_return false;
            sent += static_cast<size_t>(n);
        }
    }
    co_return true;
}

void http3_session::handle_expiry() {
    if (!impl_ || !impl_->conn_) return;
    auto rv = ngtcp2_conn_handle_expiry(impl_->conn_, now_ms() * 1000000ULL);
    if (rv != 0) {
        impl_->alive_ = false;
    }
    impl_->write_packets();
}

int64_t http3_session::get_expiry() const {
    if (!impl_ || !impl_->conn_) return -1;
    auto expiry = ngtcp2_conn_get_expiry(impl_->conn_);
    if (expiry == UINT64_MAX) return -1;
    auto now = now_ms() * 1000000ULL;
    if (expiry <= now) return 0;
    return static_cast<int64_t>((expiry - now) / 1000000);
}

void http3_session::set_request_handler(request_handler handler) {
    if (impl_) impl_->request_handler_ = std::move(handler);
}

int32_t http3_session::submit_request(const request& req,
                                       std::shared_ptr<response_promise> promise) {
    if (!impl_ || !impl_->conn_) return -1;

    // If handshake not complete, queue the request
    if (!impl_->handshake_done_) {
        impl_->pending_request_ = req;
        impl_->pending_promise_ = promise;
        impl_->has_pending_request_ = true;
        return 0; // queued, will submit after handshake
    }

    // Build H3 headers
    std::vector<std::pair<std::string, std::string>> hdrs;
    hdrs.push_back({":method", std::string(to_string(req.method))});
    hdrs.push_back({":path", req.path.empty() ? "/" : req.path});
    hdrs.push_back({":scheme", "https"});

    // Extract :authority from headers
    std::string authority;
    for (auto& [k, v] : req.hdrs) {
        if (k == ":authority" || k == "Host" || k == "host") {
            authority = v;
        }
    }
    if (!authority.empty()) {
        hdrs.push_back({":authority", authority});
    }
    for (auto& [k, v] : req.hdrs) {
        if (k[0] != ':') {
            hdrs.push_back({k, v});
        }
    }

    // Encode with QPACK
    std::string qpack_data = impl_->qpack_encoder_.encode_static(hdrs);

    // Build H3 frames
    std::string frames = h3::build_headers_frame(qpack_data);
    if (!req.bd.empty()) {
        frames.append(h3::build_data_frame(req.bd.data()));
    }

    // Open a bidirectional stream
    int64_t stream_id;
    auto rv = ngtcp2_conn_open_bidi_stream(impl_->conn_, &stream_id, nullptr);
    if (rv != 0) {
        return -3;
    }

    // Track the stream and associate the promise
    auto& ss = impl_->streams_[stream_id];
    ss.stream_id = stream_id;
    ss.state = impl::stream_state::open;
    ss.send_buf = std::move(frames);
    ss.promise = promise;

    // Write stream data
    uint8_t pkt_buf[1472];
    ngtcp2_vec datavec;
    datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(ss.send_buf.data()));
    datavec.len = ss.send_buf.size();
    ngtcp2_ssize pdatalen = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        impl_->conn_, nullptr, nullptr,
        pkt_buf, sizeof(pkt_buf),
        &pdatalen,
        NGTCP2_WRITE_STREAM_FLAG_FIN,
        stream_id, &datavec, 1,
        now_ms() * 1000000ULL);

    if (nwrite > 0) {
        impl_->output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
    }

    // Write any remaining handshake/padding packets
    impl_->write_packets();

    return static_cast<int32_t>(stream_id);
}

void http3_session::impl::submit_queued_request() {
    if (!has_pending_request_) return;
    has_pending_request_ = false;

    // Build H3 headers
    std::vector<std::pair<std::string, std::string>> hdrs;
    hdrs.push_back({":method", std::string(to_string(pending_request_.method))});
    hdrs.push_back({":path", pending_request_.path.empty() ? "/" : pending_request_.path});
    hdrs.push_back({":scheme", "https"});

    std::string authority;
    for (auto& [k, v] : pending_request_.hdrs) {
        if (k == ":authority" || k == "Host" || k == "host") {
            authority = v;
        }
    }
    if (!authority.empty()) {
        hdrs.push_back({":authority", authority});
    }
    for (auto& [k, v] : pending_request_.hdrs) {
        if (k[0] != ':') {
            hdrs.push_back({k, v});
        }
    }

    std::string qpack_data = qpack_encoder_.encode_static(hdrs);
    std::string frames = h3::build_headers_frame(qpack_data);
    if (!pending_request_.bd.empty()) {
        frames.append(h3::build_data_frame(pending_request_.bd.data()));
    }

    int64_t stream_id;
    auto rv = ngtcp2_conn_open_bidi_stream(conn_, &stream_id, nullptr);
    if (rv != 0) {
        if (pending_promise_) {
            pending_promise_->error = true;
            pending_promise_->complete = true;
        }
        return;
    }

    auto& ss = streams_[stream_id];
    ss.stream_id = stream_id;
    ss.state = stream_state::open;
    ss.send_buf = std::move(frames);
    ss.promise = pending_promise_;

    uint8_t pkt_buf[1472];
    ngtcp2_vec datavec;
    datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(ss.send_buf.data()));
    datavec.len = ss.send_buf.size();
    ngtcp2_ssize pdatalen = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        conn_, nullptr, nullptr,
        pkt_buf, sizeof(pkt_buf),
        &pdatalen,
        NGTCP2_WRITE_STREAM_FLAG_FIN,
        stream_id, &datavec, 1,
        now_ms() * 1000000ULL);

    if (nwrite > 0) {
        output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
    }

    write_packets();
}

std::vector<std::shared_ptr<http3_session::response_promise>>
http3_session::take_completed_promises() {
    if (!impl_) return {};
    auto result = std::move(impl_->completed_promises_);
    impl_->completed_promises_.clear();
    return result;
}

void* http3_session::native_handle() const {
    return impl_ ? impl_->conn_ : nullptr;
}

void http3_session::set_push_provider(push_provider provider) {
    if (impl_) impl_->push_provider_ = std::move(provider);
}

int64_t http3_session::submit_push(const request& promised_req, const response& push_resp,
                                    int64_t associated_stream_id) {
    if (!impl_) return -1;
    return impl_->do_submit_push(promised_req, push_resp, associated_stream_id);
}

void http3_session::set_push_handler(push_handler handler) {
    if (impl_) impl_->push_handler_ = std::move(handler);
}

// --- Push implementations ---

void http3_session::impl::handle_push_promise(int64_t /*stream_id*/, const std::string& payload) {
    if (mode_ != mode::client) return;

    auto pp = h3::parse_push_promise(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    // Decode the promised request headers with QPACK
    auto headers = qpack_decoder_.decode(
        reinterpret_cast<const uint8_t*>(pp.header_block.data()),
        pp.header_block.size());

    http3_session::push_promise_info info;
    info.push_id = pp.push_id;
    for (auto& [name, value] : headers) {
        if (name == ":method") {
            auto m = parse_method(value);
            if (m) info.promised_request.method = *m;
        } else if (name == ":path") {
            info.promised_request.path = value;
        } else if (name == ":scheme" || name == ":authority") {
            if (name == ":authority") info.promised_request.hdrs.append("Host", value);
        } else if (name[0] != ':') {
            info.promised_request.hdrs.append(name, value);
        }
    }
    info.promised_request.ver = version::HTTP_3;

    // Notify via push handler
    if (push_handler_) {
        push_handler_(info);
    }
}

void http3_session::impl::handle_push_stream(int64_t stream_id, const uint8_t* data,
                                              size_t datalen, bool fin) {
    if (mode_ != mode::client) return;

    auto& ss = streams_[stream_id];
    ss.stream_id = stream_id;

    // Push stream format after stream type byte:
    // Push ID (varint) + H3 HEADERS frame + H3 DATA frame(s)
    // The stream type byte was already consumed by the caller.
    // We need to parse: Push ID varint, then H3 frames.

    // Feed data into the stream's frame reader
    // But first we need to extract the Push ID if not yet done
    if (!ss.headers_complete && ss.body.empty()) {
        // First data on this push stream: extract Push ID
        uint64_t push_id;
        int n = h3::decode_varint(data, datalen, push_id);
        if (n <= 0) return; // Need more data

        // Store push_id in stream state (use send_offset as scratch)
        ss.send_offset = push_id;

        // Feed remaining data to frame reader
        size_t remaining = datalen - n;
        if (remaining > 0) {
            ss.reader.feed(data + n, remaining);
        }
    } else {
        ss.reader.feed(data, datalen);
    }

    // Process H3 frames
    while (ss.reader.has_frame()) {
        auto f = ss.reader.take_frame();
        if (f.type == h3::frame_type::HEADERS) {
            // Decode response headers
            auto headers = qpack_decoder_.decode(
                reinterpret_cast<const uint8_t*>(f.payload.data()), f.payload.size());
            for (auto& [name, value] : headers) {
                if (name == ":status") {
                    int code = 0;
                    std::from_chars(value.data(), value.data() + value.size(), code);
                    ss.resp.status = status_code(code);
                } else if (name[0] != ':') {
                    ss.resp.hdrs.append(name, value);
                }
            }
            ss.resp.ver = version::HTTP_3;
            ss.headers_complete = true;
        } else if (f.type == h3::frame_type::DATA) {
            ss.resp_body.append(f.payload);
        }
    }

    if (fin && ss.headers_complete) {
        // Push stream complete — create a completed promise
        auto promise = std::make_shared<http3_session::response_promise>();
        promise->is_push = true;
        promise->push_info.push_id = static_cast<uint64_t>(ss.send_offset);
        promise->resp = std::move(ss.resp);
        promise->resp.bd = body(std::move(ss.resp_body));
        promise->complete = true;
        completed_promises_.push_back(promise);
    }
}

int64_t http3_session::impl::do_submit_push(const request& promised_req,
                                             const response& push_resp,
                                             int64_t associated_stream_id) {
    if (mode_ != mode::server || !conn_) return -1;

    uint64_t push_id = next_push_id_++;

    // 1. Send MAX_PUSH_ID on control stream (if needed)
    if (push_id >= max_push_id_sent_) {
        uint64_t new_max = push_id + 10; // Allow batch of 10 pushes
        std::string max_push_data = h3::build_max_push_id_frame(new_max);

        // Write to control stream
        uint8_t pkt_buf[1472];
        ngtcp2_vec datavec;
        datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(max_push_data.data()));
        datavec.len = max_push_data.size();
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            control_stream_id_, &datavec, 1,
            now_ms() * 1000000ULL);
        if (nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
        }
        max_push_id_sent_ = new_max;
    }

    // 2. Send PUSH_PROMISE on the associated request stream
    std::vector<std::pair<std::string, std::string>> push_hdrs;
    push_hdrs.emplace_back(":method", to_string(promised_req.method));
    push_hdrs.emplace_back(":path", promised_req.path.empty() ? "/" : promised_req.path);
    push_hdrs.emplace_back(":scheme", "https");
    auto host = promised_req.hdrs.get("Host").value_or("localhost");
    push_hdrs.emplace_back(":authority", host);
    for (auto& [k, v] : promised_req.hdrs) {
        std::string lk = k;
        std::transform(lk.begin(), lk.end(), lk.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (lk == "host" || lk == "connection") continue;
        push_hdrs.emplace_back(lk, v);
    }
    std::string qpack_data = qpack_encoder_.encode_static(push_hdrs);
    std::string pp_frame = h3::build_push_promise_frame(push_id, qpack_data);

    // Write PUSH_PROMISE to the associated stream
    {
        uint8_t pkt_buf[1472];
        ngtcp2_vec datavec;
        datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(pp_frame.data()));
        datavec.len = pp_frame.size();
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            associated_stream_id, &datavec, 1,
            now_ms() * 1000000ULL);
        if (nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
        }
    }

    // 3. Open a new unidirectional push stream
    int64_t push_stream_id;
    auto rv = ngtcp2_conn_open_uni_stream(conn_, &push_stream_id, nullptr);
    if (rv != 0) return -1;

    // Build push stream data: stream type (PUSH=0x01) + Push ID + HEADERS frame + DATA frame
    std::string push_data;
    h3::append_varint(push_data, static_cast<uint64_t>(h3::stream_type::PUSH));
    h3::append_varint(push_data, push_id);

    // Response headers
    std::vector<std::pair<std::string, std::string>> resp_hdrs;
    resp_hdrs.push_back({":status", std::to_string(push_resp.status.as_int())});
    for (auto& [k, v] : push_resp.hdrs) {
        resp_hdrs.push_back({k, v});
    }
    std::string resp_qpack = qpack_encoder_.encode_static(resp_hdrs);
    push_data.append(h3::build_headers_frame(resp_qpack));

    // Response body
    if (!push_resp.bd.empty()) {
        push_data.append(h3::build_data_frame(push_resp.bd.data()));
    }

    // Write push stream data with FIN
    {
        uint8_t pkt_buf[1472];
        ngtcp2_vec datavec;
        datavec.base = reinterpret_cast<uint8_t*>(const_cast<char*>(push_data.data()));
        datavec.len = push_data.size();
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn_, nullptr, nullptr,
            pkt_buf, sizeof(pkt_buf),
            &pdatalen,
            NGTCP2_WRITE_STREAM_FLAG_FIN,
            push_stream_id, &datavec, 1,
            now_ms() * 1000000ULL);
        if (nwrite > 0) {
            output_pkts_.emplace_back(reinterpret_cast<const char*>(pkt_buf), nwrite);
        }
    }

    write_packets();
    return static_cast<int64_t>(push_id);
}

} // namespace http
} // namespace async_net
