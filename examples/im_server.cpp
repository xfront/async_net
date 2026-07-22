// IM Server — Instant Messaging with long-polling push & offline message storage
//
// Demonstrates: HTTP server, coroutine-based request handling, long-polling
// pattern for real-time message delivery, offline message queue.
//
// API:
//   POST /api/login?user=alice          — user goes online
//   POST /api/logout?user=alice         — user goes offline
//   POST /api/send?from=alice&to=bob    — send a message (body = text)
//   GET  /api/poll?user=bob             — long-poll for new messages
//   GET  /api/offline?user=bob          — fetch & clear offline messages
//   GET  /api/status                    — show online users & stats
//
// When a message is sent to an offline user, it is stored. When the user
// comes back online and polls, stored messages are delivered (server push
// via long-polling — the pending GET /api/poll is resolved immediately).
//
// Usage: ./im_server [port]

#include <async_net/http/server.hpp>
#include <async_net/http/client.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/coroutine/spawn.hpp>
#include <async_net/executor/schedule.hpp>
#include <iostream>
#include <csignal>
#include <queue>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <sstream>

using namespace async_net;
using namespace async_net::http;
using namespace std::chrono;

static bool g_running = true;
static void sig_handler(int) { g_running = false; }

// ---------------------------------------------------------------------------
// IM State — shared message store, presence, and long-poll waiters
// ---------------------------------------------------------------------------

struct Message {
    std::string from;
    std::string to;
    std::string text;
    steady_clock::time_point timestamp;
};

class ImState {
public:
    // Login: mark user online
    void login(const std::string& user) {
        online_users_.insert(user);
        std::cout << "[IM] " << user << " came online (online=" << online_users_.size() << ")" << std::endl;
    }

    // Logout: mark user offline
    void logout(const std::string& user) {
        online_users_.erase(user);
        std::cout << "[IM] " << user << " went offline (online=" << online_users_.size() << ")" << std::endl;
    }

    bool is_online(const std::string& user) const {
        return online_users_.count(user) > 0;
    }

    // Send a message. If recipient is online, deliver immediately via pending poll.
    // If offline, store in offline queue.
    bool send_message(const std::string& from, const std::string& to, const std::string& text) {
        Message msg{from, to, text, steady_clock::now()};
        total_messages_++;

        if (is_online(to)) {
            // Recipient is online — try to deliver via pending long-poll
            auto it = pending_polls_.find(to);
            if (it != pending_polls_.end()) {
                // Resolve the pending poll with the message
                it->second->messages.push_back(msg);
                it->second->resolved = true;
                pending_polls_.erase(it);
                std::cout << "[IM] " << from << " -> " << to << ": \"" << text << "\" (delivered via push)" << std::endl;
            } else {
                // Online but no pending poll — queue for next poll
                inbox_[to].push_back(msg);
                std::cout << "[IM] " << from << " -> " << to << ": \"" << text << "\" (queued)" << std::endl;
            }
            return true;
        } else {
            // Recipient is offline — store for later
            offline_queue_[to].push_back(msg);
            std::cout << "[IM] " << from << " -> " << to << ": \"" << text << "\" (stored offline)" << std::endl;
            return false;
        }
    }

    // Get messages from inbox (non-blocking)
    std::vector<Message> get_inbox(const std::string& user) {
        auto it = inbox_.find(user);
        if (it == inbox_.end()) return {};
        auto msgs = std::move(it->second);
        inbox_.erase(it);
        return msgs;
    }

    // Get and clear offline messages
    std::vector<Message> get_offline(const std::string& user) {
        auto it = offline_queue_.find(user);
        if (it == offline_queue_.end()) return {};
        auto msgs = std::move(it->second);
        offline_queue_.erase(it);
        return msgs;
    }

    // Register a long-poll waiter. Returns shared state that will be filled
    // when a message arrives, or nullptr if messages are already available.
    struct PollState {
        std::vector<Message> messages;
        bool resolved = false;
    };

    std::shared_ptr<PollState> register_poll(const std::string& user) {
        // If there are already messages in inbox, return them immediately
        auto it = inbox_.find(user);
        if (it != inbox_.end() && !it->second.empty()) {
            auto state = std::make_shared<PollState>();
            state->messages = std::move(it->second);
            state->resolved = true;
            inbox_.erase(it);
            return state;
        }
        // No messages — register a pending poll
        auto state = std::make_shared<PollState>();
        pending_polls_[user] = state;
        return state;
    }

    // Cancel a pending poll (e.g. on logout)
    void cancel_poll(const std::string& user) {
        pending_polls_.erase(user);
    }

    const std::set<std::string>& online_users() const { return online_users_; }
    size_t total_messages() const { return total_messages_; }

private:
    std::set<std::string> online_users_;
    std::map<std::string, std::vector<Message>> inbox_;          // online user inbox
    std::map<std::string, std::vector<Message>> offline_queue_;  // offline message store
    std::map<std::string, std::shared_ptr<PollState>> pending_polls_;  // long-poll waiters
    size_t total_messages_ = 0;
};

static ImState g_im;

// ---------------------------------------------------------------------------
// Helper: serialize messages to JSON-like text
// ---------------------------------------------------------------------------
static std::string messages_to_text(const std::vector<Message>& msgs) {
    std::string out;
    for (auto& m : msgs) {
        auto epoch = duration_cast<seconds>(m.timestamp.time_since_epoch()).count();
        out += "[" + m.from + " -> " + m.to + " @ " + std::to_string(epoch) + "] " + m.text + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Server setup
// ---------------------------------------------------------------------------
static void setup_im_routes(server& srv) {
    // POST /api/login?user=xxx
    srv.route(method::POST, "/api/login", [](const request& req) -> Task<response> {
        auto user = req.query_param("user");
        if (user.empty()) {
            co_return response_make().status(status_code::bad_request())
                .body("Missing user parameter").build();
        }
        g_im.login(user);
        co_return response_ok("OK: " + user + " is now online");
    });

    // POST /api/logout?user=xxx
    srv.route(method::POST, "/api/logout", [](const request& req) -> Task<response> {
        auto user = req.query_param("user");
        if (user.empty()) {
            co_return response_make().status(status_code::bad_request())
                .body("Missing user parameter").build();
        }
        g_im.cancel_poll(user);
        g_im.logout(user);
        co_return response_ok("OK: " + user + " is now offline");
    });

    // POST /api/send?from=xxx&to=yyy  body=message text
    srv.route(method::POST, "/api/send", [](const request& req) -> Task<response> {
        auto from_user = req.query_param("from");
        auto to_user = req.query_param("to");
        if (from_user.empty() || to_user.empty()) {
            co_return response_make().status(status_code::bad_request())
                .body("Missing from/to parameters").build();
        }
        bool delivered = g_im.send_message(from_user, to_user, req.bd.data());
        std::string status = delivered ? "delivered" : "stored_offline";
        co_return response_ok(status);
    });

    // GET /api/poll?user=xxx  — long-poll (waits up to 5s for a message)
    srv.route(method::GET, "/api/poll", [](const request& req) -> Task<response> {
        auto user = req.query_param("user");
        if (user.empty()) {
            co_return response_make().status(status_code::bad_request())
                .body("Missing user parameter").build();
        }

        auto poll_state = g_im.register_poll(user);
        if (poll_state->resolved) {
            // Messages already available — return immediately (server push!)
            co_return response_make()
                .status(status_code::ok())
                .header("X-Push", "true")
                .header("Content-Type", "text/plain")
                .body(messages_to_text(poll_state->messages))
                .build();
        }

        // Long-poll: wait up to 5 seconds for a message
        auto deadline = steady_clock::now() + seconds(5);
        while (!poll_state->resolved && steady_clock::now() < deadline) {
            co_await sleep_for(milliseconds(200));
        }

        if (poll_state->resolved) {
            co_return response_make()
                .status(status_code::ok())
                .header("X-Push", "true")
                .header("Content-Type", "text/plain")
                .body(messages_to_text(poll_state->messages))
                .build();
        }

        // Timeout — no messages
        g_im.cancel_poll(user);
        co_return response_make()
            .status(status_code::ok())
            .header("X-Push", "timeout")
            .body("")
            .build();
    });

    // GET /api/offline?user=xxx — fetch and clear offline messages
    srv.route(method::GET, "/api/offline", [](const request& req) -> Task<response> {
        auto user = req.query_param("user");
        if (user.empty()) {
            co_return response_make().status(status_code::bad_request())
                .body("Missing user parameter").build();
        }
        auto msgs = g_im.get_offline(user);
        if (msgs.empty()) {
            co_return response_ok("No offline messages");
        }
        co_return response_make()
            .status(status_code::ok())
            .header("Content-Type", "text/plain")
            .body("Offline messages (" + std::to_string(msgs.size()) + "):\n" + messages_to_text(msgs))
            .build();
    });

    // GET /api/status — server status
    srv.route(method::GET, "/api/status", [](const request& req) -> Task<response> {
        std::string body = "=== IM Server Status ===\n";
        body += "Online users: " + std::to_string(g_im.online_users().size()) + "\n";
        for (auto& u : g_im.online_users()) body += "  - " + u + "\n";
        body += "Total messages: " + std::to_string(g_im.total_messages()) + "\n";
        co_return response_ok(body);
    });

    // Default: 404
    srv.default_handler([](const request& req) -> Task<response> {
        co_return response_not_found();
    });
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    try {
        io_context ctx;
        std::cout << "Backend: " << ctx.backend().name() << std::endl;

        server srv(ctx, port);
        setup_im_routes(srv);

        std::cout << "\n=== IM Server ===" << std::endl;
        std::cout << "  http://localhost:" << port << "/api/status" << std::endl;
        std::cout << "  Long-polling push enabled" << std::endl;
        std::cout << "=================\n" << std::endl;

        auto task = std::make_unique<Task<void>>(srv.serve());
        task->resume();

        ctx.run();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "IM Server stopped." << std::endl;
    return 0;
}
