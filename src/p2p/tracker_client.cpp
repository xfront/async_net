#include "../../include/async_net/p2p/tracker_client.hpp"
#include "../../include/async_net/io/buffer.hpp"
#include <sstream>

namespace async_net::p2p {

tracker_client::tracker_client(io_context& ctx)
    : ctx_(&ctx), sock_(ctx) {}

Task<bool> tracker_client::connect(const char* tracker_host, uint16_t tracker_port,
                                   const std::string& peer_id, uint16_t local_udp_port) {
    // Connect TCP socket to tracker
    int err = co_await sock_.async_connect(tracker_host, tracker_port);
    if (err != 0) {
        std::fprintf(stderr, "[tracker_client] connect to %s:%d failed\n",
                     tracker_host, tracker_port);
        co_return false;
    }

    my_id_ = peer_id;

    // Send REGISTER command
    std::string cmd = "REGISTER " + peer_id + " " + std::to_string(local_udp_port);
    if (!co_await write_line(cmd)) {
        co_return false;
    }

    // Read response: OK <ip>:<port>
    auto response = co_await read_line();
    if (response.empty() || response.substr(0, 3) != "OK ") {
        std::fprintf(stderr, "[tracker_client] register failed: %s\n", response.c_str());
        co_return false;
    }

    // Parse public endpoint from response
    public_endpoint_ = response.substr(3);
    connected_ = true;

    std::fprintf(stderr, "[tracker_client] registered as '%s', public: %s\n",
                 peer_id.c_str(), public_endpoint_.c_str());
    co_return true;
}

Task<std::vector<peer_info>> tracker_client::list_peers() {
    std::vector<peer_info> result;
    if (!connected_) co_return result;

    if (!co_await write_line("LIST")) {
        connected_ = false;
        co_return result;
    }

    // Read: PEERS <count>\n
    auto header = co_await read_line();
    if (header.empty() || header.substr(0, 6) != "PEERS ") {
        co_return result;
    }

    int count = std::stoi(header.substr(6));
    for (int i = 0; i < count; ++i) {
        auto line = co_await read_line();
        if (line.empty()) break;

        std::istringstream iss(line);
        peer_info info;
        iss >> info.id >> info.public_address >> info.udp_port;
        if (!info.id.empty()) {
            result.push_back(std::move(info));
        }
    }

    co_return result;
}

Task<bool> tracker_client::ping() {
    if (!connected_) co_return false;

    if (!co_await write_line("PING " + my_id_)) {
        connected_ = false;
        co_return false;
    }

    auto response = co_await read_line();
    co_return (response == "PONG");
}

Task<void> tracker_client::disconnect() {
    if (connected_) {
        co_await write_line("QUIT");
        connected_ = false;
    }
    sock_.close();
}

Task<std::string> tracker_client::read_line() {
    std::string line;
    char buf[1];
    while (line.size() < 4096) {
        auto n = co_await sock_.async_read_some(mutable_buffer(buf, 1));
        if (n <= 0) {
            connected_ = false;
            co_return "";
        }
        if (buf[0] == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            co_return line;
        }
        line += buf[0];
    }
    co_return "";
}

Task<bool> tracker_client::write_line(const std::string& line) {
    std::string data = line + "\n";
    auto n = co_await sock_.async_write(buffer(data));
    co_return n == data.size();
}

} // namespace async_net::p2p
