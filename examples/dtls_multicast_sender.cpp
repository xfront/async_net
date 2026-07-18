// DTLS Multicast Sender — secure multicast with DTLS key exchange + AES-GCM data.
//
// Architecture (two-channel):
//   Control channel (DTLS unicast):  certificate auth + AES-GCM group key distribution
//   Data channel (AES-GCM multicast): encrypted data to all receivers
//
// Usage: ./dtls_multicast_sender [group] [data_port] [control_port] [cert] [key]
//   group:       multicast group address  (default 239.0.0.1)
//   data_port:   UDP port for multicast   (default 5000)
//   control_port: DTLS port for key exchange (default 5001)
//   cert:        server certificate PEM   (default examples/server_cert.pem)
//   key:         server private key PEM   (default examples/server_key.pem)

#ifdef ASYNC_NET_HAS_SSL

#include <async_net/coroutine/task.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/net/udp.hpp>
#include <async_net/net/ssl.hpp>
#include <async_net/net/dtls.hpp>
#include <async_net/crypto/aes_gcm.hpp>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

using namespace async_net;
using namespace async_net::crypto;
using namespace async_net::net;

// ---- Sender coroutine ----

static std::atomic<bool> g_running{true};

Task<void> run_sender(io_context& ctx,
                       const char* group, uint16_t data_port, uint16_t control_port,
                       const char* cert_file, const char* key_file)
{
    // ---- 1. Generate random AES-256 group key ----
    auto group_key = aes_gcm::random_bytes(aes_gcm::KEY_LEN);
    if (group_key.size() != aes_gcm::KEY_LEN) {
        fprintf(stderr, "Failed to generate group key\n");
        co_return;
    }

    printf("Generated AES-256 group key (%zu bytes)\n", group_key.size());

    // ---- 2. DTLS control: raw blocking UDP socket + dtls_stream ----
    ssl::context dtls_ctx("dtls_server");
    if (!dtls_ctx.use_certificate_file(cert_file)) {
        fprintf(stderr, "Failed to load certificate: %s\n", cert_file);
        co_return;
    }
    if (!dtls_ctx.use_private_key_file(key_file)) {
        fprintf(stderr, "Failed to load private key: %s\n", key_file);
        co_return;
    }

    // Create a raw blocking UDP socket for DTLS control channel
    int ctrl_fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (ctrl_fd < 0) {
        fprintf(stderr, "Failed to create control socket\n");
        co_return;
    }
    {
        int opt = 1;
        setsockopt(ctrl_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(ctrl_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    }
    {
        struct sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(control_port);
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        if (::bind(ctrl_fd, reinterpret_cast<struct sockaddr*>(&bind_addr),
                   sizeof(bind_addr)) != 0) {
            fprintf(stderr, "Failed to bind control port %u\n", control_port);
            ::close(ctrl_fd);
            co_return;
        }
    }

    // ---- 3. Multicast data socket ----
    udp::socket data_sock(ctx);
    if (!data_sock.is_open()) {
        fprintf(stderr, "Failed to create data socket\n");
        co_return;
    }
    data_sock.set_multicast_ttl(1);
    data_sock.set_multicast_loopback(true);
    data_sock.set_multicast_interface("127.0.0.1");

    udp::endpoint data_dest(data_port, group);

    printf("DTLS Multicast Sender\n");
    printf("  Control: DTLS on port %u\n", control_port);
    printf("  Data:    AES-GCM multicast to %s:%u\n", group, data_port);
    printf("Waiting for receivers...\n");

    // ---- 4. Control thread: handle DTLS connections & distribute group key ----

    std::thread ctrl_thread([&]() {
        while (g_running.load()) {
            // Wait for incoming data on control socket
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ctrl_fd, &rfds);
            struct timeval tv = {1, 0};
            int sel = select(ctrl_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue; // timeout or error

            // Create DTLS stream for this receiver
            dtls_stream stream(ctrl_fd, dtls_ctx, /*is_server=*/true);

            // Detect peer address from incoming packet
            if (stream.set_peer_from_socket() != 0) continue;

            // DTLS handshake (certificate authentication)
            if (stream.handshake() != 0) {
                fprintf(stderr, "[Control] DTLS handshake failed\n");
                continue;
            }
            printf("[Control] DTLS handshake OK\n");

            // Send group key over encrypted DTLS channel
            int sent = stream.write(group_key.data(), group_key.size());
            if (sent > 0) {
                printf("[Control] Sent group key (%d bytes)\n", sent);
            } else {
                fprintf(stderr, "[Control] Failed to send group key\n");
            }

            stream.shutdown();
            printf("[Control] Ready for next receiver\n");
        }
    });

    // ---- 5. Data loop: encrypt and multicast (coroutine) ----
    uint32_t seq = 0;
    uint64_t nonce_counter = 1;

    while (g_running.load()) {
        ++seq;

        // Build plaintext message
        char plaintext[256];
        int pt_len = snprintf(plaintext, sizeof(plaintext),
                              "Multicast message #%u (seq=%u)", seq, seq);

        // Build 12-byte nonce from 64-bit counter (zero-padded)
        uint8_t nonce[aes_gcm::IV_LEN] = {};
        for (int i = 0; i < 8; ++i)
            nonce[i] = static_cast<uint8_t>(nonce_counter >> (i * 8));

        // AAD = 4-byte sequence number (big-endian)
        uint8_t aad[4];
        aad[0] = static_cast<uint8_t>((seq >> 24) & 0xFF);
        aad[1] = static_cast<uint8_t>((seq >> 16) & 0xFF);
        aad[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        aad[3] = static_cast<uint8_t>(seq & 0xFF);

        // Encrypt: returns ciphertext + 16-byte GCM tag
        auto encrypted = aes_gcm::encrypt(
            group_key.data(), group_key.size(),
            nonce, sizeof(nonce),
            reinterpret_cast<const uint8_t*>(plaintext), pt_len,
            aad, sizeof(aad));

        if (encrypted.empty()) {
            fprintf(stderr, "Encryption failed\n");
            co_return;
        }

        // Build wire packet: [seq:4][nonce:12][ciphertext+tag:N]
        std::vector<uint8_t> packet(4 + aes_gcm::IV_LEN + encrypted.size());
        memcpy(packet.data(), aad, 4);
        memcpy(packet.data() + 4, nonce, aes_gcm::IV_LEN);
        memcpy(packet.data() + 4 + aes_gcm::IV_LEN, encrypted.data(), encrypted.size());

        auto n = co_await data_sock.async_send_to(
            const_buffer(packet.data(), packet.size()), data_dest);
        if (n > 0) {
            printf("Sent encrypted: %s (%zu bytes wire)\n", plaintext, packet.size());
        } else {
            fprintf(stderr, "Send failed: %zd\n", n);
        }

        ++nonce_counter;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    g_running.store(false);
    ctrl_thread.join();
    co_return;
}

int main(int argc, char* argv[]) {
    const char* group = (argc > 1) ? argv[1] : "239.0.0.1";
    uint16_t data_port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 5000;
    uint16_t control_port = (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 5001;
    const char* cert = (argc > 4) ? argv[4] : "examples/server_cert.pem";
    const char* key = (argc > 5) ? argv[5] : "examples/server_key.pem";

    try {
        io_context ctx;
        auto* task = new Task<void>(run_sender(ctx, group, data_port, control_port, cert, key));
        task->resume();
        if (task->done()) { delete task; return 0; }
        ctx.run();
        delete task;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}

#else
#include <cstdio>
int main() {
    fprintf(stderr, "Built without SSL support\n");
    return 1;
}
#endif // ASYNC_NET_HAS_SSL
