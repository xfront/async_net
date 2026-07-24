#include "../../include/async_net/p2p/tracker_server.hpp"
#include "../../include/async_net/net/buffer.hpp"
#include "../../include/async_net/executor/schedule.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <async_net/detail/config.hpp>
#ifndef ASYNC_NET_WINDOWS
#include <arpa/inet.h>
#endif

namespace async_net::p2p {

tracker_server::tracker_server(io_context& ctx, uint16_t port)
    : ctx_(&ctx) {
    acceptor_ = std::make_unique<tcp::acceptor>(ctx, port);
}

Task<void> tracker_server::run() {
    if (!acceptor_ || !acceptor_->is_open()) {
        std::fprintf(stderr, "[tracker] acceptor not ready\n");
        co_return;
    }

    running_ = true;
    std::fprintf(stderr, "[tracker] server started\n");

    // Background cleanup task (heap-allocated to stay alive)
    auto* cleanup_task = new Task<void>([this]() -> Task<void> {
        while (running_) {
            co_await sleep_for(std::chrono::seconds(10), *ctx_);
            cleanup_stale_peers();
        }
    }());
    cleanup_task->resume();

    while (running_) {
        auto sock = co_await acceptor_->async_accept();
        if (!sock.is_open()) {
            if (!running_) break;
            continue;
        }

        // Handle each client in a heap-allocated coroutine
        auto* task = new Task<void>(handle_client(std::move(sock)));
        {
            std::lock_guard lock(tasks_mutex_);
            active_tasks_.insert(task);
        }
        task->resume();

        // Clean up completed tasks
        std::lock_guard lock(tasks_mutex_);
        for (auto it = active_tasks_.begin(); it != active_tasks_.end(); ) {
            if ((*it)->done()) {
                delete *it;
                it = active_tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::vector<peer_info> tracker_server::peers() const {
    std::lock_guard lock(peers_mutex_);
    std::vector<peer_info> result;
    result.reserve(peers_.size());
    for (const auto& [id, info] : peers_) {
        result.push_back(info);
    }
    return result;
}

void tracker_server::set_timeout(std::chrono::seconds t) {
    peer_timeout_ = t;
}

void tracker_server::stop() {
    running_ = false;
    if (acceptor_) {
        acceptor_->close();
    }
}

void tracker_server::cleanup_stale_peers() {
    std::lock_guard lock(peers_mutex_);
    auto now = std::chrono::steady_clock::now();
    auto it = peers_.begin();
    while (it != peers_.end()) {
        if (now - it->second.last_seen > peer_timeout_) {
            std::fprintf(stderr, "[tracker] peer '%s' timed out\n", it->first.c_str());
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
}

Task<std::string> tracker_server::read_line(tcp::socket& sock) {
    std::string line;
    char buf[1];
    while (line.size() < 1024) {
        auto n = co_await sock.async_read_some(mutable_buffer(buf, 1));
        if (n <= 0) {
            co_return "";  // Connection closed or error
        }
        if (buf[0] == '\n') {
            // Remove trailing \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            co_return line;
        }
        line += buf[0];
    }
    co_return "";  // Line too long
}

Task<bool> tracker_server::write_line(tcp::socket& sock, const std::string& line) {
    std::string data = line + "\n";
    auto n = co_await sock.async_write(buffer(data));
    co_return n == data.size();
}

Task<void> tracker_server::handle_client(tcp::socket sock) {
    int fd = sock.native_handle();
    std::string peer_id;

    // Get peer's address from socket
    struct sockaddr_in peer_addr{};
    socklen_t peer_len = sizeof(peer_addr);
    ::getpeername(fd, reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len);
    char peer_ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
    std::string client_ip(peer_ip);

    while (running_) {
        auto line = co_await read_line(sock);
        if (line.empty()) {
            break;  // Connection closed
        }

        // Parse command
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "REGISTER") {
            std::string id;
            uint16_t udp_port = 0;
            uint16_t tcp_port = 0;
            iss >> id >> udp_port;
            if (!(iss >> tcp_port)) tcp_port = 0;

            if (id.empty() || udp_port == 0) {
                co_await write_line(sock, "ERROR invalid REGISTER format");
                continue;
            }

            // Register peer with observed public address
            peer_info info;
            info.id = id;
            info.public_address = client_ip;
            info.udp_port = udp_port;
            info.tcp_port = tcp_port;
            info.last_seen = std::chrono::steady_clock::now();

            {
                std::lock_guard lock(peers_mutex_);
                peers_[id] = info;
            }

            // Track connection -> peer_id mapping
            {
                std::lock_guard lock(conn_mutex_);
                connection_peers_[fd] = id;
            }
            peer_id = id;

            std::fprintf(stderr, "[tracker] peer '%s' registered from %s:%d\n",
                         id.c_str(), client_ip.c_str(), udp_port);

            // Respond with observed public endpoint
            std::string response = "OK " + client_ip + ":" + std::to_string(ntohs(peer_addr.sin_port));
            co_await write_line(sock, response);

        } else if (cmd == "LIST") {
            std::vector<peer_info> peer_list;
            {
                std::lock_guard lock(peers_mutex_);
                for (auto& [id, info] : peers_) {
                    info.last_seen = std::chrono::steady_clock::now();
                    peer_list.push_back(info);
                }
            }

            std::string response = "PEERS " + std::to_string(peer_list.size()) + "\n";
            for (const auto& p : peer_list) {
                if (p.id == peer_id) continue;  // Don't list self
                response += p.id + " " + p.public_address + " " + std::to_string(p.udp_port) + "\n";
            }
            co_await write_line(sock, response);

        } else if (cmd == "PING") {
            std::string id;
            iss >> id;

            // Update last_seen for this peer
            if (!peer_id.empty()) {
                std::lock_guard lock(peers_mutex_);
                auto it = peers_.find(peer_id);
                if (it != peers_.end()) {
                    it->second.last_seen = std::chrono::steady_clock::now();
                }
            }

            co_await write_line(sock, "PONG");

        } else if (cmd == "QUIT") {
            break;
        } else {
            co_await write_line(sock, "ERROR unknown command");
        }
    }

    // Cleanup: remove peer from registry
    if (!peer_id.empty()) {
        {
            std::lock_guard lock(peers_mutex_);
            peers_.erase(peer_id);
        }
        {
            std::lock_guard lock(conn_mutex_);
            connection_peers_.erase(fd);
        }
        std::fprintf(stderr, "[tracker] peer '%s' disconnected\n", peer_id.c_str());
    }
}

} // namespace async_net::p2p
