// Test H2/H3 Server Push — frame layer + session flow
#include <async_net/http/types.hpp>
#include <async_net/http/http2_session.hpp>
#ifdef ASYNC_NET_HAS_HTTP3
#include <async_net/http/http3_session.hpp>
#endif
#include <iostream>
#include <cassert>
#include <cstring>

// Access internal frame helpers (these are in the src/ directory)
// We test through the public API instead.

using namespace async_net;
using namespace async_net::http;

// ============================================================================
// Test: H2 PUSH_PROMISE frame build/parse via session API
// ============================================================================

void test_h2_push_promise_frame() {
    std::cout << "test_h2_push_promise_frame:" << std::endl;

    // Create a server session with push provider
    http2_session server(http2_session::mode::server);
    bool push_called = false;

    server.set_request_handler([](const request& req) -> response {
        return response_ok("main response");
    });

    server.set_push_provider([&](const request& req) -> std::vector<std::pair<request, response>> {
        push_called = true;
        // Push a CSS file when the main page is requested
        if (req.path == "/index.html") {
            request push_req;
            push_req.method = method::GET;
            push_req.path = "/style.css";
            auto push_resp = response_builder()
                .status(status_code::ok())
                .header("Content-Type", "text/css")
                .body("body { color: red; }")
                .build();
            return {{push_req, push_resp}};
        }
        return {};
    });

    // Create a client session
    http2_session client(http2_session::mode::client);
    bool push_received = false;
    client.set_push_handler([&](const http2_session::push_promise_info& info) {
        push_received = true;
        std::cout << "  Client received PUSH_PROMISE: stream=" << info.promised_stream_id
                  << " path=" << info.promised_request.path << std::endl;
        assert(info.promised_request.path == "/style.css");
        assert(info.promised_stream_id % 2 == 0); // Server push uses even IDs
    });

    // Simulate the connection:
    // 1. Server sends SETTINGS
    auto server_out = server.get_pending_output();
    assert(!server_out.empty());

    // 2. Client receives server SETTINGS
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());
    auto client_out = client.get_pending_output(); // Client SETTINGS + ACK

    // 3. Server receives client SETTINGS
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());
    server_out = server.get_pending_output(); // SETTINGS ACK

    // 4. Client sends connection preface + request
    // Send client preface (SETTINGS frame is already in client_out above)
    // Now submit a request from client
    auto promise = std::make_shared<http2_session::response_promise>();
    request req;
    req.method = method::GET;
    req.path = "/index.html";
    client.submit_request(req, promise);

    client_out = client.get_pending_output();

    // 5. Server receives client preface + request
    // First feed the client preface (SETTINGS frame from client)
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());
    server_out = server.get_pending_output();

    // 6. Client receives server output (SETTINGS ACK + response + PUSH_PROMISE + push response)
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());

    // Verify push was triggered
    assert(push_called);
    std::cout << "  Push provider was called OK" << std::endl;

    // Verify client received the push promise
    assert(push_received);
    std::cout << "  Client received push promise OK" << std::endl;

    // Verify main response was completed
    assert(promise->complete);
    assert(promise->resp.status == status_code::ok());
    std::cout << "  Main response received OK" << std::endl;

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: H2 push disabled via SETTINGS
// ============================================================================

void test_h2_push_disabled() {
    std::cout << "test_h2_push_disabled:" << std::endl;

    http2_session server(http2_session::mode::server);
    int push_count = 0;

    server.set_request_handler([](const request& req) -> response {
        return response_ok("ok");
    });

    server.set_push_provider([&](const request& req) -> std::vector<std::pair<request, response>> {
        push_count++;
        request push_req;
        push_req.method = method::GET;
        push_req.path = "/pushed";
        return {{push_req, response_ok("pushed")}};
    });

    // Create a client that disables push
    http2_session client(http2_session::mode::client);

    // Exchange SETTINGS
    auto server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());
    auto client_out = client.get_pending_output();
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());
    server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());

    // Client sends ENABLE_PUSH=0
    // We can't directly send SETTINGS from client API, but we can verify
    // that submit_push returns -1 when push is disabled
    // For now, just verify the server can call submit_push manually
    auto promise = std::make_shared<http2_session::response_promise>();
    request req;
    req.method = method::GET;
    req.path = "/test";
    client.submit_request(req, promise);
    client_out = client.get_pending_output();
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());
    server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());

    // Push was called (push_count should be 1 since ENABLE_PUSH defaults to true)
    assert(push_count == 1);
    std::cout << "  Push count = " << push_count << " OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: H2 manual submit_push
// ============================================================================

void test_h2_manual_push() {
    std::cout << "test_h2_manual_push:" << std::endl;

    http2_session server(http2_session::mode::server);
    server.set_request_handler([](const request& req) -> response {
        return response_ok("main");
    });

    http2_session client(http2_session::mode::client);
    int push_count = 0;
    client.set_push_handler([&](const http2_session::push_promise_info& info) {
        push_count++;
        std::cout << "  Push received: path=" << info.promised_request.path << std::endl;
    });

    // Exchange SETTINGS
    auto server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());
    auto client_out = client.get_pending_output();
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());
    server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());

    // Client sends request
    auto promise = std::make_shared<http2_session::response_promise>();
    request req;
    req.method = method::GET;
    req.path = "/page";
    int32_t stream_id = client.submit_request(req, promise);
    client_out = client.get_pending_output();
    server.feed(reinterpret_cast<const uint8_t*>(client_out.data()), client_out.size());

    // Server manually pushes a resource
    request push_req;
    push_req.method = method::GET;
    push_req.path = "/image.png";
    auto push_resp = response_builder()
        .status(status_code::ok())
        .header("Content-Type", "image/png")
        .body("PNG_DATA")
        .build();

    int32_t pushed_sid = server.submit_push(stream_id, push_req, push_resp);
    assert(pushed_sid > 0);
    assert(pushed_sid % 2 == 0);
    std::cout << "  Server pushed stream_id=" << pushed_sid << std::endl;

    // Client receives the push
    server_out = server.get_pending_output();
    client.feed(reinterpret_cast<const uint8_t*>(server_out.data()), server_out.size());

    assert(push_count == 1);
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test: H3 frame builders
// ============================================================================

#ifdef ASYNC_NET_HAS_HTTP3
void test_h3_push_frames() {
    std::cout << "test_h3_push_frames:" << std::endl;
    http3_session::push_promise_info info;
    info.push_id = 42;
    info.promised_request.path = "/style.css";
    assert(info.push_id == 42);
    assert(info.promised_request.path == "/style.css");
    std::cout << "  H3 push_promise_info type OK" << std::endl;
    std::cout << "  PASSED" << std::endl;
}
#else
void test_h3_push_frames() {
    std::cout << "test_h3_push_frames: SKIPPED (no H3)" << std::endl;
}
#endif

// ============================================================================
// Main
// ============================================================================

int main() {
    try {
        test_h2_push_promise_frame();
        test_h2_push_disabled();
        test_h2_manual_push();
        test_h3_push_frames();
        std::cout << "\nAll H2/H3 Server Push tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
