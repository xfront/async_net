// IM Client Simulator — multi-user IM simulation with random behavior
//
// Simulates multiple virtual users that:
//   - Come online at random intervals
//   - Send messages to random users at random intervals
//   - Go offline at random intervals
//   - Fetch offline messages when coming back online
//   - Use long-polling for real-time message delivery (server push)
//
// Each user runs as a coroutine, demonstrating:
//   - co_spawn / spawn for concurrent user sessions
//   - sleep_for for random delays
//   - http::client for HTTP requests
//   - Long-polling pattern for push-based message delivery
//
// Usage: ./im_client_sim [server_host] [server_port] [num_users] [duration_sec]
//
// Example:
//   # Terminal 1: start the IM server
//   ./im_server 8080
//
//   # Terminal 2: start the client simulator
//   ./im_client_sim localhost 8080 8 30
//   # → 8 virtual users, run for 30 seconds

#include <async_net/http/client.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>
#include <async_net/coroutine/spawn.hpp>
#include <async_net/executor/coroutine.hpp>
#include <async_net/executor/schedule.hpp>
#include <iostream>
#include <csignal>
#include <random>
#include <vector>
#include <string>
#include <chrono>
#include <sstream>
#include <atomic>

using namespace async_net;
using namespace async_net::http;
using namespace std::chrono;

static bool g_running = true;
static void sig_handler(int) { g_running = false; }

// ---------------------------------------------------------------------------
// User names and message pool
// ---------------------------------------------------------------------------
static const std::vector<std::string> g_user_names = {
    "alice", "bob", "charlie", "diana", "eve",
    "frank", "grace", "henry", "iris", "jack",
    "kate", "leo", "mia", "nick", "olivia"
};

static const std::vector<std::string> g_messages = {
    "Hello!", "How are you?", "What's up?", "Good morning!",
    "See you later", "Can we meet?", "Great idea!", "Sure thing",
    "Let me check...", "On my way!", "Thanks!", "No problem",
    "Happy birthday!", "LOL", "Interesting...", "I agree",
    "Let's do it!", "Good night!", "Bye!", "Miss you!",
    "Check this out", "Wow!", "Really?", "That's awesome",
    "Need help?", "I'm busy", "Call me", "Sounds good"
};

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
struct Stats {
    std::atomic<int> messages_sent{0};
    std::atomic<int> messages_received{0};
    std::atomic<int> offline_messages{0};
    std::atomic<int> logins{0};
    std::atomic<int> logouts{0};
};
static Stats g_stats;

// ---------------------------------------------------------------------------
// Helper: build URL
// ---------------------------------------------------------------------------
static std::string base_url;

static std::string make_url(const std::string& path,
                             const std::string& param_key = "",
                             const std::string& param_val = "",
                             const std::string& param_key2 = "",
                             const std::string& param_val2 = "") {
    std::string url = base_url + path;
    if (!param_key.empty()) {
        url += "?" + param_key + "=" + param_val;
        if (!param_key2.empty()) {
            url += "&" + param_key2 + "=" + param_val2;
        }
    }
    return url;
}

// ---------------------------------------------------------------------------
// User coroutine — simulates one user's behavior
// Each user has its own http::client to avoid concurrent access issues.
// ---------------------------------------------------------------------------
Task<void> simulate_user(io_context& ctx,
                          const std::string& username,
                          std::vector<std::string> all_users,
                          steady_clock::time_point end_time,
                          int seed) {
    std::mt19937 rng(seed);
    auto rand_delay = [&](int min_ms, int max_ms) -> milliseconds {
        std::uniform_int_distribution<int> dist(min_ms, max_ms);
        return milliseconds(dist(rng));
    };
    auto pick_random = [&](const std::vector<std::string>& v) -> const std::string& {
        std::uniform_int_distribution<int> dist(0, static_cast<int>(v.size()) - 1);
        return v[dist(rng)];
    };

    // Remove self from recipient list
    all_users.erase(std::remove(all_users.begin(), all_users.end(), username), all_users.end());

    auto tag = "[" + username + "]";
    client cli(ctx);  // Each user has its own client

    while (g_running && steady_clock::now() < end_time) {
        // --- Phase 1: Come online ---
        std::cout << tag << " Logging in..." << std::endl;
        auto resp = co_await cli.post(make_url("/api/login", "user", username), "");
        if (resp.status.as_int() == 200) {
            g_stats.logins++;
        }

        // Check for offline messages
        auto off_resp = co_await cli.get(make_url("/api/offline", "user", username));
        if (off_resp.status.as_int() == 200 && off_resp.bd.data().find("Offline messages") != std::string::npos) {
            std::cout << tag << " Fetched offline: " << off_resp.bd.data().substr(0, 80) << "..." << std::endl;
            g_stats.offline_messages++;
        }

        // --- Phase 2: Online period — alternate between poll and send ---
        auto online_duration = rand_delay(4000, 10000);  // 4-10 seconds online
        auto online_end = steady_clock::now() + online_duration;

        while (g_running && steady_clock::now() < online_end) {
            // Poll for messages (server long-polls up to 5s, returns immediately if msg available)
            auto poll_resp = co_await cli.get(make_url("/api/poll", "user", username));
            if (poll_resp.status.as_int() == 200) {
                auto push = poll_resp.hdrs.get("X-Push").value_or("");
                if (push == "true" && !poll_resp.bd.empty()) {
                    std::cout << tag << " << Received (push): "
                              << poll_resp.bd.data().substr(0, 60) << std::endl;
                    g_stats.messages_received++;
                }
            }

            if (!g_running || steady_clock::now() >= online_end) break;

            // Small delay then send a message
            co_await sleep_for(rand_delay(200, 800), ctx);
            if (!g_running || steady_clock::now() >= online_end) break;

            const auto& recipient = pick_random(all_users);
            const auto& msg_text = pick_random(g_messages);

            auto send_resp = co_await cli.post(
                make_url("/api/send", "from", username, "to", recipient),
                msg_text);

            if (send_resp.status.as_int() == 200) {
                std::cout << tag << " >> Sent to " << recipient << ": \"" << msg_text << "\""
                          << " (" << send_resp.bd.data() << ")" << std::endl;
                g_stats.messages_sent++;
            }
        }

        // --- Phase 3: Go offline ---
        resp = co_await cli.post(make_url("/api/logout", "user", username), "");
        if (resp.status.as_int() == 200) {
            g_stats.logouts++;
        }
        std::cout << tag << " Logged out" << std::endl;

        // Wait before coming back online
        co_await sleep_for(rand_delay(2000, 5000), ctx);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    std::string host = "localhost";
    uint16_t port = 8080;
    int num_users = 6;
    int duration_sec = 30;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) num_users = std::atoi(argv[3]);
    if (argc > 4) duration_sec = std::atoi(argv[4]);

    base_url = "http://" + host + ":" + std::to_string(port);

    std::cout << "\n=== IM Client Simulator ===" << std::endl;
    std::cout << "  Server:  " << base_url << std::endl;
    std::cout << "  Users:   " << num_users << std::endl;
    std::cout << "  Duration: " << duration_sec << "s" << std::endl;
    std::cout << "===========================\n" << std::endl;

    // Select users from the pool
    int actual_users = std::min(num_users, static_cast<int>(g_user_names.size()));
    std::vector<std::string> users(g_user_names.begin(), g_user_names.begin() + actual_users);

    try {
        io_context ctx;

        auto end_time = steady_clock::now() + seconds(duration_sec);

        // Spawn one coroutine per user (each creates its own http::client)
        std::vector<std::unique_ptr<Task<void>>> tasks;
        for (int i = 0; i < actual_users; ++i) {
            auto t = std::make_unique<Task<void>>(
                simulate_user(ctx, users[i], g_user_names, end_time, 42 + i));
            t->resume();
            tasks.push_back(std::move(t));
        }

        // Run until duration expires or Ctrl-C
        auto monitor = std::make_unique<Task<void>>([&]() -> Task<void> {
            while (g_running && steady_clock::now() < end_time) {
                co_await sleep_for(seconds(5), ctx);
                std::cout << "\n--- Stats: sent=" << g_stats.messages_sent.load()
                          << " recv=" << g_stats.messages_received.load()
                          << " offline=" << g_stats.offline_messages.load()
                          << " logins=" << g_stats.logins.load()
                          << " logouts=" << g_stats.logouts.load()
                          << " ---\n" << std::endl;
            }
            g_running = false;
            ctx.stop();
        }());
        monitor->resume();

        ctx.run();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n=== Final Stats ===" << std::endl;
    std::cout << "  Messages sent:     " << g_stats.messages_sent.load() << std::endl;
    std::cout << "  Messages received: " << g_stats.messages_received.load() << std::endl;
    std::cout << "  Offline deliveries: " << g_stats.offline_messages.load() << std::endl;
    std::cout << "  User logins:       " << g_stats.logins.load() << std::endl;
    std::cout << "  User logouts:      " << g_stats.logouts.load() << std::endl;
    std::cout << "===================\n" << std::endl;

    return 0;
}
