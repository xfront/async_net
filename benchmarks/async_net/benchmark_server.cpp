// TechEmpower Framework Benchmarks — async_net implementation
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
// Multi-process architecture for multi-core utilization:
//   - Uses SO_REUSEPORT to allow multiple processes to bind the same port
//   - Each worker process has its own io_context and server instance
//   - Kernel distributes incoming connections across workers (load balancing)
//   - Each worker has its own copy of the in-memory database (no lock contention)

#include <async_net/http/server.hpp>
#include <async_net/io/io_context.hpp>
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>
#include <cstring>
#include <charconv>
#include <iostream>
#include <ctime>
#include <thread>
#include <signal.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

using namespace async_net;
using namespace async_net::http;

// ============================================================================
// Cached Date header (set once at startup per TechEmpower guidelines)
// ============================================================================

static std::string g_cached_date;

static void update_date_cache() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
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

// World table: 10,000 rows
static std::vector<World> g_world_table;

// Fortune table: 12 standard fortunes
static std::vector<Fortune> g_fortune_table;

// CachedWorld table (same as World, for caching test)
static std::vector<World> g_cached_world_table;

// Thread-local RNG
static thread_local std::mt19937 g_rng{std::random_device{}()};

static int random_id() {
    std::uniform_int_distribution<int> dist(1, 10000);
    return dist(g_rng);
}

static int random_number() {
    std::uniform_int_distribution<int> dist(1, 10000);
    return dist(g_rng);
}

// Initialize in-memory database
static void init_database() {
    // World table: 10,000 rows
    g_world_table.reserve(10000);
    for (int i = 1; i <= 10000; ++i) {
        g_world_table.push_back({i, random_number()});
    }

    // CachedWorld table: same initial data
    g_cached_world_table = g_world_table;

    // Fortune table: 12 standard TechEmpower fortunes
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
        {12, "??????????????"}  // Japanese fortune (UTF-8)
    };
}

// ============================================================================
// JSON helpers (minimal, no external dependency)
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

// HTML-escape a string for XSS protection
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

// Parse query parameter value, bounded to [1, max_val]
static int parse_queries_param(const std::string& query, const std::string& param_name, int max_val) {
    // Find param_name= in query string
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

// Test 1: JSON Serialization
// GET /json → {"message":"Hello, World!"}
static Task<response> handle_json(const request& /*req*/) {
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body("{\"message\":\"Hello, World!\"}")
        .build();
}

// Test 6: Plaintext
// GET /plaintext → Hello, World!
static Task<response> handle_plaintext(const request& /*req*/) {
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "text/plain")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body("Hello, World!")
        .build();
}

// Test 2: Single Database Query
// GET /db → {"id":N,"randomNumber":N}
static Task<response> handle_db(const request& /*req*/) {
    int id = random_id();
    const World& w = g_world_table[id - 1];  // ids are 1-based
    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(world_to_json(w))
        .build();
}

// Test 3: Multiple Database Queries
// GET /queries?queries=N → [{"id":N,"randomNumber":N}, ...]
static Task<response> handle_queries(const request& req) {
    int query_count = parse_queries_param(req.query, "queries", 500);

    std::vector<World> worlds;
    worlds.reserve(query_count);
    for (int i = 0; i < query_count; ++i) {
        int id = random_id();
        worlds.push_back(g_world_table[id - 1]);
    }

    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(worlds_to_json(worlds))
        .build();
}

// Test 4: Fortunes
// GET /fortunes → HTML table
static Task<response> handle_fortunes(const request& /*req*/) {
    // Copy fortunes and add new one
    std::vector<Fortune> fortunes = g_fortune_table;
    fortunes.push_back({0, "Additional fortune added at request time."});

    // Sort by message
    std::sort(fortunes.begin(), fortunes.end(),
              [](const Fortune& a, const Fortune& b) { return a.message < b.message; });

    // Render HTML
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

// Test 5: Database Updates
// GET /updates?queries=N → [{"id":N,"randomNumber":N}, ...]
static Task<response> handle_updates(const request& req) {
    int query_count = parse_queries_param(req.query, "queries", 500);

    std::vector<World> worlds;
    worlds.reserve(query_count);
    for (int i = 0; i < query_count; ++i) {
        int id = random_id();
        World w = g_world_table[id - 1];
        w.randomNumber = random_number();
        // Update in-memory table
        g_world_table[id - 1].randomNumber = w.randomNumber;
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

// Test 7: Caching
// GET /cached-queries?count=N → [{"id":N,"randomNumber":N}, ...]
static Task<response> handle_cached_queries(const request& req) {
    int count = parse_queries_param(req.query, "count", 500);

    std::vector<World> worlds;
    worlds.reserve(count);
    for (int i = 0; i < count; ++i) {
        int id = random_id();
        worlds.push_back(g_cached_world_table[id - 1]);
    }

    co_return response_make()
        .status(status_code::ok())
        .header("Content-Type", "application/json")
        .header("Server", "async_net")
        .header("Date", get_date())
        .body(worlds_to_json(worlds))
        .build();
}

// ============================================================================
// Route setup
// ============================================================================

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
// Worker process (runs one io_context + server)
// ============================================================================

static void run_worker(uint16_t port, int worker_id) {
    // Each worker has its own io_context and server
    // With SO_REUSEPORT, kernel distributes connections across workers
    io_context ctx;

    // Enable SO_REUSEPORT for multi-process binding to same port
    server srv(ctx, port, "0.0.0.0", /*reuse_port=*/true);
    setup_routes(srv);

    if (worker_id == 0) {
        std::cout << "Backend: " << ctx.backend().name() << std::endl;
    }

    auto task = srv.serve();
    task.resume();
    ctx.run();
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    // Determine number of workers (default: number of CPU cores)
    unsigned int num_workers = std::thread::hardware_concurrency();
    if (num_workers == 0) num_workers = 4;  // fallback

    // Parse optional -w N argument
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            num_workers = static_cast<unsigned int>(std::atoi(argv[++i]));
            if (num_workers == 0) num_workers = 1;
        }
    }

    // Initialize in-memory database and date cache (before fork)
    init_database();
    update_date_cache();

    std::cout << "\n=== async_net TechEmpower Benchmark Server ===" << std::endl;
    std::cout << "  Port: " << port << std::endl;
    std::cout << "  Workers: " << num_workers << " (using SO_REUSEPORT)" << std::endl;
    std::cout << "  Tests:" << std::endl;
    std::cout << "    GET /json                    — JSON Serialization" << std::endl;
    std::cout << "    GET /plaintext               — Plaintext" << std::endl;
    std::cout << "    GET /db                      — Single DB Query" << std::endl;
    std::cout << "    GET /queries?queries=N       — Multiple DB Queries (1-500)" << std::endl;
    std::cout << "    GET /fortunes                — Fortunes" << std::endl;
    std::cout << "    GET /updates?queries=N       — DB Updates (1-500)" << std::endl;
    std::cout << "    GET /cached-queries?count=N  — Caching (1-500)" << std::endl;
    std::cout << "==============================================\n" << std::endl;

#ifdef _WIN32
    // Windows: single worker (no fork support)
    std::cout << "[Windows] Running single worker (fork not supported)" << std::endl;
    try {
        run_worker(port, 0);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
#else
    // Unix: multi-process with SO_REUSEPORT
    if (num_workers == 1) {
        // Single worker mode
        try {
            run_worker(port, 0);
        } catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
            return 1;
        }
    } else {
        // Multi-worker mode: fork worker processes
        std::vector<pid_t> child_pids;
        child_pids.reserve(num_workers);

        // Handle SIGCHLD to reap zombie processes
        signal(SIGCHLD, SIG_IGN);

        for (unsigned int i = 0; i < num_workers; ++i) {
            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "fork() failed" << std::endl;
                // Kill existing children
                for (pid_t p : child_pids) {
                    kill(p, SIGTERM);
                }
                return 1;
            } else if (pid == 0) {
                // Child process
                try {
                    run_worker(port, static_cast<int>(i));
                } catch (const std::exception& e) {
                    std::cerr << "Worker " << i << " exception: " << e.what() << std::endl;
                }
                _exit(0);
            } else {
                // Parent process
                child_pids.push_back(pid);
                std::cout << "[master] Spawned worker " << i << " (pid=" << pid << ")" << std::endl;
            }
        }

        std::cout << "[master] All " << num_workers << " workers running" << std::endl;

        // Wait for children (will be interrupted by signals)
        while (true) {
            int status;
            pid_t pid = wait(&status);
            if (pid < 0) {
                if (errno == ECHILD) break;  // No more children
                if (errno == EINTR) continue;  // Interrupted by signal
                break;
            }
            std::cout << "[master] Worker pid=" << pid << " exited" << std::endl;
        }
    }
#endif

    return 0;
}
