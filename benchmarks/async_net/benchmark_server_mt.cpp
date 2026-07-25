// TechEmpower Framework Benchmarks — async_net implementation (Multi-threaded version)
// Implements all 7 test types:
//   1. JSON Serialization   — GET /json
//   2. Single DB Query      — GET /db
//   3. Multiple DB Queries  — GET /queries?queries=N
//   4. Fortunes             — GET /fortunes
//   5. DB Updates           — GET /updates?queries=N
//   6. Plaintext            — GET /plaintext
//   7. Caching              — GET /cached-queries?count=N
//
// Database tables (World, Fortune) are simulated in-memory.
// Uses async_net HTTP server with C++20 coroutines.
//
// Multi-threaded architecture for multi-core utilization:
//   Linux/macOS: SO_REUSEPORT - each thread has its own server binding same port
//   Windows: Shared acceptor - multiple io_context instances share one acceptor,
//            IOCP distributes completed accepts across waiting threads

#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include <cstring>
#include <charconv>
#include <iostream>
#include <ctime>
#include <mutex>
#include <memory>

#include <async_net/http/server.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/io/tcp.hpp>
#include <async_net/executor/process_manager.hpp>

using namespace async_net;
using namespace async_net::http;

// ============================================================================
// Cached Date header (set once at startup per TechEmpower guidelines)
// ============================================================================

static std::string g_cached_date;

static void update_date_cache() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf;
#ifdef ASYNC_NET_WINDOWS
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M %S GMT", &tm_buf);
    g_cached_date = buf;
}

static const std::string& get_date() {
    return g_cached_date;
}

// ============================================================================
// In-memory database simulation (TechEmpower schema)
// ============================================================================

struct World {
    int id;
    int randomNumber;
};

struct Fortune {
    int id;
    std::string message;
};

// World table: 10,000 rows (read-only after init, shared across threads)
static std::vector<World> g_world_table;

// Fortune table: 12 standard fortunes (read-only, shared)
static std::vector<Fortune> g_fortune_table;

// CachedWorld table (read-only after init, shared)
static std::vector<World> g_cached_world_table;

// Thread-local RNG (each thread has its own RNG, no contention)
static thread_local std::mt19937 g_rng{std::random_device{}()};

// Thread-local World table for updates (each thread has its own copy)
static thread_local std::vector<World> g_local_world_table;
static thread_local bool g_local_world_initialized = false;

static void init_local_world_table() {
    if (!g_local_world_initialized) {
        g_local_world_table = g_world_table;
        g_local_world_initialized = true;
    }
}

static int random_id() {
    std::uniform_int_distribution<int> dist(1, 10000);
    return dist(g_rng);
}

static int random_number() {
    std::uniform_int_distribution<int> dist(1, 10000);
    return dist(g_rng);
}

// Initialize in-memory database (called once before threads start)
static void init_database() {
    g_world_table.reserve(10000);
    for (int i = 1; i <= 10000; ++i) {
        g_world_table.push_back({i, random_number()});
    }
    g_cached_world_table = g_world_table;
    g_fortune_table = {
        {1,  "fortune: No such file or directory"},
        {2,  "A computer scientist is someone who fixes things that aren't broken."},
        {3,  "After enough decimal places, nobody gives a damn."},
        {4,  "A bad random number generator: 1, 1, 1, 1, 1, 4.33e+67, 1, 1, 1"},
        {5,  "A computer program does what you tell it to do, not what you want it to do."},
        {6,  "Emacs is a nice operating system, but I prefer UNIX. — Tom Christaensen"},
        {7,  "Any program that runs right is obsolete."},
        {8,  "A list is only as strong as its weakest link. — Donald Knuth"},
        {9,  "Feature: A bug with seniority."},
        {10, "Computers make very fast, very accurate mistakes."},
        {11, "<script>alert(\"This should not be displayed in a browser alert box.\");</script>"},
        {12, "??????????????"}
    };
}

// ============================================================================
// JSON helpers
// ============================================================================

static std::string world_to_json(const World& w) {
    return "{\"id\":" + std::to_string(w.id) + ",\"randomNumber\":" + std::to_string(w.randomNumber) + "}";
}

static std::string worlds_to_json(const std::vector<World>& worlds) {
    std::string result = "[";
    for (size_t i = 0; i < worlds.size(); ++i) {
        if (i > 0) result += ",";
        result += world_to_json(worlds[i]);
    }
    result += "]";
    return result;
}

static std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

static int parse_queries_param(const std::string& query, const std::string& param_name, int max_val) {
    std::string search = param_name + "=";
    auto pos = query.find(search);
    if (pos == std::string::npos) return 1;
    pos += search.size();
    int value = 0;
    auto [ptr, ec] = std::from_chars(query.data() + pos, query.data() + query.size(), value);
    if (ec != std::errc()) return 1;
    if (value < 1) return 1;
    if (value > max_val) return max_val;
    return value;
}

// ============================================================================
// TechEmpower test handlers
// ============================================================================

static Task<response> handle_json(const request&) {
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body("{\"message\":\"Hello, World!\"}")
        .build();
}

static Task<response> handle_plaintext(const request&) {
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "text/plain")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body("Hello, World!")
        .build();
}

static Task<response> handle_db(const request&) {
    int id = random_id();
    const World& w = g_world_table[id - 1];
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(world_to_json(w))
        .build();
}

static Task<response> handle_queries(const request& req) {
    int query_count = parse_queries_param(req.query, "queries", 500);
    std::vector<World> worlds;
    worlds.reserve(query_count);
    for (int i = 0; i < query_count; ++i) {
        worlds.push_back(g_world_table[random_id() - 1]);
    }
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(worlds_to_json(worlds))
        .build();
}

static Task<response> handle_fortunes(const request&) {
    std::vector<Fortune> fortunes = g_fortune_table;
    fortunes.push_back({0, "Additional fortune added at request time."});
    std::sort(fortunes.begin(), fortunes.end(),
              [](const Fortune& a, const Fortune& b) { return a.message < b.message; });
    std::string html;
    html.reserve(2048);
    html += "<!DOCTYPE html><html><head><title>Fortunes</title></head>"
            "<body><table><tr><th>id</th><th>message</th></tr>";
    for (const auto& f : fortunes) {
        html += "<tr><td>" + std::to_string(f.id) + "</td><td>" + html_escape(f.message) + "</td></tr>";
    }
    html += "</table></body></html>";
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "text/html; charset=utf-8")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(std::move(html))
        .build();
}

static Task<response> handle_updates(const request& req) {
    init_local_world_table();
    int query_count = parse_queries_param(req.query, "queries", 500);
    std::vector<World> worlds;
    worlds.reserve(query_count);
    for (int i = 0; i < query_count; ++i) {
        int id = random_id();
        World& w = g_local_world_table[id - 1];
        w.randomNumber = random_number();
        worlds.push_back(w);
    }
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(worlds_to_json(worlds))
        .build();
}

static Task<response> handle_cached_queries(const request& req) {
    int count = parse_queries_param(req.query, "count", 500);
    std::vector<World> worlds;
    worlds.reserve(count);
    for (int i = 0; i < count; ++i) {
        worlds.push_back(g_cached_world_table[random_id() - 1]);
    }
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(worlds_to_json(worlds))
        .build();
}

static void setup_routes(server& srv) {
    srv.route(method::GET, "/json",             handle_json);
    srv.route(method::GET, "/plaintext",        handle_plaintext);
    srv.route(method::GET, "/db",               handle_db);
    srv.route(method::GET, "/queries",          handle_queries);
    srv.route(method::GET, "/fortunes",         handle_fortunes);
    srv.route(method::GET, "/updates",          handle_updates);
    srv.route(method::GET, "/cached-queries",   handle_cached_queries);
    srv.default_handler([](const request& req) -> Task<response> {
        co_return response_make()
            .status(status_code::not_found())
            .header("Server", "async_net")
            .body("404 Not Found: " + req.path)
            .build();
    });
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    unsigned int num_workers = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            num_workers = static_cast<unsigned int>(std::atoi(argv[++i]));
        }
    }

    init_database();
    update_date_cache();

    std::cout << "\n=== async_net TechEmpower Benchmark Server (Multi-threaded) ===" << std::endl;
    std::cout << "  Port: " << port << std::endl;
    std::cout << "  Workers: " << num_workers << std::endl;
    std::cout << "  Tests:" << std::endl;
    std::cout << "    GET /json                    — JSON Serialization" << std::endl;
    std::cout << "    GET /plaintext               — Plaintext" << std::endl;
    std::cout << "    GET /db                      — Single DB Query" << std::endl;
    std::cout << "    GET /queries?queries=N       — Multiple DB Queries (1-500)" << std::endl;
    std::cout << "    GET /fortunes                — Fortunes" << std::endl;
    std::cout << "    GET /updates?queries=N       — DB Updates (1-500)" << std::endl;
    std::cout << "    GET /cached-queries?count=N  — Caching (1-500)" << std::endl;
    std::cout << "=============================================================\n" << std::endl;

    // Worker function: each worker gets its own io_context + server + config
    auto worker = [port](int worker_id, io_context& ctx, const default_worker_config&) -> Task<void> {

        server srv(ctx, port, "0.0.0.0", /*reuse_port=*/true);
        setup_routes(srv);

        if (worker_id == 0) {
            std::cout << "Backend: " << ctx.backend().name() << std::endl;
        }

        co_await srv.serve();
    };

    // Run with nginx-style master-worker model
    default_worker_config cfg;
    cfg.mode = worker_mode::thread;
    cfg.num_workers = num_workers;
    return run_mp_master(std::move(worker), cfg);
}
