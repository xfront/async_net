// gRPC Echo Server — demonstrates unary, server-streaming, client-streaming,
// and bidi-streaming RPCs with Protocol Buffers serialization.
//
// Message definitions: examples/grpc/echo.proto (generated via protoc)

#include <async_net/grpc/server.hpp>
#include <async_net/io/io_context.hpp>
#include "echo.pb.h"
#include <iostream>
#include <string>

using namespace async_net;
using namespace async_net::grpc;

// ============================================================================
// Unary handlers
// ============================================================================

Task<std::string> echo_handler(const std::string& request_data) {
    echo::EchoRequest req;
    req.ParseFromString(request_data);
    std::cout << "[echo] Received: " << req.message() << std::endl;

    echo::EchoResponse resp;
    resp.set_message(req.message());

    std::string serialized;
    resp.SerializeToString(&serialized);
    co_return serialized;
}

Task<std::string> hello_handler(const std::string& request_data) {
    echo::HelloRequest req;
    req.ParseFromString(request_data);
    std::cout << "[hello] Received name: " << req.name() << std::endl;

    echo::HelloResponse resp;
    resp.set_greeting("Hello, " + req.name() + "!");

    std::string serialized;
    resp.SerializeToString(&serialized);
    co_return serialized;
}

// ============================================================================
// Server-streaming handler: sends N echo responses
// ============================================================================

Task<void> handle_server_stream(const std::string& request_data, writer& w) {
    echo::EchoRequest req;
    req.ParseFromString(request_data);
    std::cout << "[server-stream] Request: " << req.message() << std::endl;

    echo::EchoResponse resp;
    for (int i = 1; i <= 3; ++i) {
        resp.set_message("Response #" + std::to_string(i) + ": " + req.message());
        std::cout << "[server-stream] Sending: " << resp.message() << std::endl;

        std::string serialized;
        resp.SerializeToString(&serialized);
        co_await w.write(serialized);
    }

    co_await w.finish();
}

// ============================================================================
// Client-streaming handler: receives multiple messages, returns combined result
// ============================================================================

Task<std::string> handle_client_stream(reader& r) {
    std::string combined;
    int count = 0;

    while (auto msg = co_await r.read()) {
        echo::EchoRequest req;
        req.ParseFromString(*msg);
        std::cout << "[client-stream] Received: " << req.message() << std::endl;
        if (!combined.empty()) combined += ", ";
        combined += req.message();
        ++count;
    }

    echo::EchoResponse resp;
    resp.set_message("Received " + std::to_string(count) + " messages: [" + combined + "]");
    std::cout << "[client-stream] Final: " << resp.message() << std::endl;

    std::string serialized;
    resp.SerializeToString(&serialized);
    co_return serialized;
}

// ============================================================================
// Bidirectional-streaming handler: echoes each message in real-time
// ============================================================================

Task<void> handle_bidi_stream(reader& r, writer& w) {
    std::cout << "[bidi-stream] Started" << std::endl;

    echo::EchoRequest req;
    echo::EchoResponse resp;

    while (auto msg = co_await r.read()) {
        req.ParseFromString(*msg);
        resp.set_message("Echo: " + req.message());
        std::cout << "[bidi-stream] " << resp.message() << std::endl;

        std::string serialized;
        resp.SerializeToString(&serialized);
        co_await w.write(serialized);
    }

    co_await w.finish();
}

// ============================================================================
// Main
// ============================================================================

Task<void> main_coroutine(io_context& ctx) {
    grpc::server srv(ctx, 50051);

    // Unary RPCs
    srv.register_method("EchoService", "Echo", echo_handler);
    srv.register_method("GreeterService", "SayHello", hello_handler);

    // Streaming RPCs
    srv.register_server_stream("EchoService", "ServerStream", handle_server_stream);
    srv.register_client_stream("EchoService", "ClientStream", handle_client_stream);
    srv.register_bidi_stream("EchoService", "BidiStream", handle_bidi_stream);

    std::cout << "gRPC Echo Server starting on port 50051..." << std::endl;
    std::cout << "Services (protobuf):" << std::endl;
    std::cout << "  - EchoService/Echo (unary)" << std::endl;
    std::cout << "  - GreeterService/SayHello (unary)" << std::endl;
    std::cout << "  - EchoService/ServerStream (server-streaming)" << std::endl;
    std::cout << "  - EchoService/ClientStream (client-streaming)" << std::endl;
    std::cout << "  - EchoService/BidiStream (bidirectional-streaming)" << std::endl;

    co_await srv.serve();
}

int main() {
    io_context ctx;

    auto task = main_coroutine(ctx);
    task.resume();

    ctx.run();

    return 0;
}
