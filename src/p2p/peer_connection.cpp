#include "../../include/async_net/p2p/peer_connection.hpp"
#include "../../include/async_net/executor/schedule.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace async_net::p2p {

peer_connection::peer_connection(io_context& ctx, ssl::context& dtls_ctx)
    : ctx_(&ctx), dtls_ctx_(dtls_ctx) {}

peer_connection::~peer_connection() {
    close();
}

static int create_nonblocking_udp_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

// Wait for socket readiness using select(), as a coroutine.
static Task<bool> wait_for_socket(int fd, bool writable,
                                  std::chrono::milliseconds timeout,
                                  io_context& ctx) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        struct timeval tv{};
        tv.tv_usec = 50000;  // 50ms poll for select
        int ret;
        if (writable)
            ret = ::select(fd + 1, nullptr, &fds, nullptr, &tv);
        else
            ret = ::select(fd + 1, &fds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(fd, &fds)) co_return true;

        if (std::chrono::steady_clock::now() >= deadline) co_return false;

        // Yield to io_context
        co_await sleep_for(std::chrono::milliseconds(5), ctx);
    }
}

Task<bool> peer_connection::connect_to(const peer_info& remote, uint16_t local_udp_port) {
    remote_ = remote;

    int fd = create_nonblocking_udp_socket(local_udp_port);
    if (fd < 0) {
        std::fprintf(stderr, "[p2p] failed to create UDP socket on port %d\n", local_udp_port);
        co_return false;
    }

    bool dtls_ok = co_await do_dtls_handshake(fd, /*is_server=*/false,
                                              remote.public_address, remote.udp_port);
    if (!dtls_ok) {
        std::fprintf(stderr, "[p2p] DTLS handshake failed with peer '%s'\n", remote.id.c_str());
        ::close(fd);
        co_return false;
    }

    connected_ = true;
    socket_fd_ = fd;
    std::fprintf(stderr, "[p2p] connected to peer '%s' (DTLS established)\n", remote.id.c_str());
    co_return true;
}

Task<bool> peer_connection::accept_from(const peer_info& remote, uint16_t local_udp_port) {
    remote_ = remote;

    int fd = create_nonblocking_udp_socket(local_udp_port);
    if (fd < 0) {
        std::fprintf(stderr, "[p2p] failed to create UDP socket on port %d\n", local_udp_port);
        co_return false;
    }

    bool dtls_ok = co_await do_dtls_handshake(fd, /*is_server=*/true,
                                              remote.public_address, remote.udp_port);
    if (!dtls_ok) {
        std::fprintf(stderr, "[p2p] DTLS handshake failed with peer '%s'\n", remote.id.c_str());
        ::close(fd);
        co_return false;
    }

    connected_ = true;
    socket_fd_ = fd;
    std::fprintf(stderr, "[p2p] connected to peer '%s' (DTLS established)\n", remote.id.c_str());
    co_return true;
}

Task<bool> peer_connection::do_dtls_handshake(int udp_fd, bool is_server,
                                              const std::string& remote_ip, uint16_t remote_port) {
    std::fprintf(stderr, "[p2p:dtls] creating DTLS stream (server=%d) on fd %d\n",
                 is_server, udp_fd);

    dtls_ = std::make_unique<net::dtls_stream>(udp_fd, dtls_ctx_, is_server);

    // For client: set peer address
    if (!is_server) {
        if (dtls_->set_peer(remote_ip.c_str(), remote_port) != 0) {
            std::fprintf(stderr, "[p2p:dtls] set_peer(%s:%d) failed\n", remote_ip.c_str(), remote_port);
            co_return false;
        }
    }

    std::fprintf(stderr, "[p2p:dtls] starting handshake...\n");
    dtls_->begin_handshake();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (true) {
        int ret = dtls_->handshake_step();

        if (ret == 0) {
            std::fprintf(stderr, "[p2p:dtls] handshake OK\n");
            co_return true;
        }

        // Check if wolfSSL wants read or write
        bool want_w = dtls_->wants_write();
        bool want_r = dtls_->wants_read();

        if (want_w || want_r) {
            // Wait for socket readiness
            bool ready = co_await wait_for_socket(udp_fd, want_w,
                                                  std::chrono::milliseconds(500), *ctx_);
            if (!ready) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::fprintf(stderr, "[p2p:dtls] handshake TIMEOUT\n");
                    co_return false;
                }
                // Retry — DTLS retransmission will happen inside wolfSSL
                continue;
            }
            continue;
        }

        // Fatal error (handshake_step already logged it)
        std::fprintf(stderr, "[p2p:dtls] handshake FAILED (ret=%d)\n", ret);
        co_return false;
    }
}

Task<bool> peer_connection::send(const std::string& msg) {
    return send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
}

Task<bool> peer_connection::send(const uint8_t* data, size_t len) {
    if (!connected_ || !dtls_) co_return false;

    peer_message pmsg;
    pmsg.type = msg_type::data;
    pmsg.payload.assign(data, data + len);
    auto wire = pmsg.serialize();

    size_t total_sent = 0;
    while (total_sent < wire.size()) {
        int n = dtls_->write(wire.data() + total_sent, wire.size() - total_sent);
        if (n > 0) {
            total_sent += n;
            continue;
        }
        if (n == 0) {
            // Would block — wait for writable
            if (!co_await wait_for_socket(socket_fd_, /*writable=*/true,
                                          std::chrono::seconds(10), *ctx_)) {
                co_return false;
            }
            continue;
        }
        // Error
        co_return false;
    }

    co_return true;
}

Task<std::optional<peer_message>> peer_connection::receive() {
    if (!connected_ || !dtls_) co_return std::nullopt;

    // Wait for readable data
    while (true) {
        if (!co_await wait_for_socket(socket_fd_, /*writable=*/false,
                                      std::chrono::seconds(60), *ctx_)) {
            connected_ = false;
            co_return std::nullopt;
        }

        uint8_t buf[65536];
        int n = dtls_->read(buf, sizeof(buf));
        if (n > 0) {
            auto msg = peer_message::deserialize(buf, n);
            co_return msg;
        }
        if (n == 0) {
            // Would block — loop and wait again
            continue;
        }
        // Error
        connected_ = false;
        co_return std::nullopt;
    }
}

void peer_connection::close() {
    if (dtls_) {
        dtls_->shutdown();
        dtls_.reset();
    }
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_ = false;
}

} // namespace async_net::p2p
