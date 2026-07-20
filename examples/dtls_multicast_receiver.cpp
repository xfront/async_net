// DTLS Multicast Receiver — joins secure multicast group.
//
// Flow:
//   1. DTLS connect to sender's control port, verify sender certificate
//   2. Receive AES-256 group key over encrypted DTLS channel
//   3. Join multicast group, receive AES-GCM encrypted data
//   4. Decrypt and print plaintext
//
// Usage: ./dtls_multicast_receiver [group] [data_port] [control_port] [ca_cert]
//   group:       multicast group address  (default 239.0.0.1)
//   data_port:   UDP port for multicast   (default 5000)
//   control_port: DTLS port for key exchange (default 5001)
//   ca_cert:     CA certificate to verify sender (default server_cert.pem)

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
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

using namespace async_net;
using namespace async_net::crypto;
using namespace async_net::net;

// ---- Receiver coroutine ----

Task<void> run_receiver(io_context& ctx,
                         const char* group, uint16_t data_port, uint16_t control_port,
                         const char* ca_cert)
{
    // ---- 1. DTLS client: connect to sender, get group key ----
    ssl::context dtls_ctx("dtls_client");
    if (!dtls_ctx.load_verify_file(ca_cert)) {
        fprintf(stderr, "Failed to load CA certificate: %s\n", ca_cert);
        co_return;
    }
    dtls_ctx.set_verify_peer(true);

    // Create a raw blocking UDP socket for DTLS control connection
    int ctrl_fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (ctrl_fd < 0) {
        fprintf(stderr, "Failed to create control socket\n");
        co_return;
    }

    // "Connect" UDP socket to sender's control port
    struct sockaddr_in sender_addr{};
    sender_addr.sin_family = AF_INET;
    sender_addr.sin_port = htons(control_port);
    if (::inet_pton(AF_INET, "127.0.0.1", &sender_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid sender address\n");
        ::close(ctrl_fd);
        co_return;
    }
    if (::connect(ctrl_fd, reinterpret_cast<struct sockaddr*>(&sender_addr),
                  sizeof(sender_addr)) != 0) {
        fprintf(stderr, "Failed to connect control socket to 127.0.0.1:%u\n", control_port);
        ::close(ctrl_fd);
        co_return;
    }

    // DTLS handshake via dtls_stream
    printf("Connecting to sender DTLS on 127.0.0.1:%u...\n", control_port);
    std::vector<uint8_t> group_key(aes_gcm::KEY_LEN);
    {
        dtls_stream stream(ctrl_fd, dtls_ctx, /*is_server=*/false);
        if (stream.handshake() != 0) {
            fprintf(stderr, "DTLS handshake failed\n");
            ::close(ctrl_fd);
            co_return;
        }
        printf("[Control] DTLS handshake OK, sender verified\n");

        // Receive group key (32 bytes)
        int key_len = stream.read(group_key.data(), group_key.size());
        if (key_len != static_cast<int>(aes_gcm::KEY_LEN)) {
            fprintf(stderr, "Failed to receive group key (got %d bytes, expected %zu)\n",
                    key_len, aes_gcm::KEY_LEN);
            stream.shutdown();
            ::close(ctrl_fd);
            co_return;
        }
        printf("[Control] Received group key (%d bytes)\n", key_len);

        stream.shutdown();
    }
    // dtls_stream destroyed, close control socket
    ::close(ctrl_fd);

    // ---- 2. Join multicast group, receive encrypted data ----
    udp::socket data_sock(ctx);
    if (!data_sock.is_open()) {
        fprintf(stderr, "Failed to create data socket\n");
        co_return;
    }

    data_sock.set_reuse_address(true);
    data_sock.set_reuse_port(true);
    data_sock.set_multicast_interface("127.0.0.1");

    udp::endpoint listen_ep(data_port);
    if (!data_sock.bind(listen_ep)) {
        fprintf(stderr, "Failed to bind data port %u\n", data_port);
        co_return;
    }

    if (!data_sock.join_multicast_group(group, "127.0.0.1")) {
        fprintf(stderr, "Failed to join multicast group %s\n", group);
        co_return;
    }
    printf("[Data] Joined multicast group %s:%u, waiting for encrypted data...\n",
           group, data_port);

    // ---- 3. Receive and decrypt packets ----
    uint8_t buf[4096];
    udp::endpoint from;

    while (true) {
        auto n = co_await data_sock.async_receive_from(buffer(buf, sizeof(buf)), from);
        if (n <= 0) {
            fprintf(stderr, "Receive error: %zd\n", n);
            break;
        }

        // Need at least: seq(4) + nonce(12) + tag(16) = 32 bytes minimum
        if (n < static_cast<ssize_t>(4 + aes_gcm::IV_LEN + aes_gcm::TAG_LEN)) {
            fprintf(stderr, "Packet too short (%zd bytes), skipping\n", n);
            continue;
        }

        // Parse wire format: [seq:4][nonce:12][ciphertext+tag:N]
        const uint8_t* seq_bytes = buf;
        const uint8_t* nonce = buf + 4;
        const uint8_t* ciphertext = buf + 4 + aes_gcm::IV_LEN;
        size_t ct_len = static_cast<size_t>(n) - 4 - aes_gcm::IV_LEN;

        uint32_t seq = (static_cast<uint32_t>(seq_bytes[0]) << 24) |
                       (static_cast<uint32_t>(seq_bytes[1]) << 16) |
                       (static_cast<uint32_t>(seq_bytes[2]) << 8) |
                       static_cast<uint32_t>(seq_bytes[3]);

        // Decrypt with AES-GCM (AAD = 4-byte seq)
        auto plaintext = aes_gcm::decrypt(
            group_key.data(), group_key.size(),
            nonce, aes_gcm::IV_LEN,
            ciphertext, ct_len,
            seq_bytes, 4);

        if (plaintext) {
            printf("[%s:%u] [seq=%u] %.*s\n",
                   group, data_port, seq,
                   static_cast<int>(plaintext->size()),
                   reinterpret_cast<const char*>(plaintext->data()));
        } else {
            fprintf(stderr, "[seq=%u] Decryption failed (auth tag mismatch)\n", seq);
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    const char* group = (argc > 1) ? argv[1] : "239.0.0.1";
    uint16_t data_port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 5000;
    uint16_t control_port = (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 5001;
    const char* ca_cert = (argc > 4) ? argv[4] : "server_cert.pem";

    try {
        io_context ctx;
        auto* task = new Task<void>(run_receiver(ctx, group, data_port, control_port, ca_cert));
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
