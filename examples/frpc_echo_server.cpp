// FRPC Echo Server — demonstrates unary, server-streaming, client-streaming,
// and bidi-streaming RPCs with FlatBuffers serialization.
//
// Message definitions: examples/frpc/echo.fbs (generated via flatc)

#include <async_net/frpc/server.hpp>
#include <async_net/io/io_context.hpp>
#include "echo_generated.h"
#include <iostream>
#include <string>

using namespace async_net;
using namespace async_net::frpc;

// Helper: serialize FlatBuffers response to string
static std::string serialize_fb(flatbuffers::FlatBufferBuilder& builder) {
    return std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                       builder.GetSize());
}

// ============================================================================
// Unary handlers
// ============================================================================

Task<std::string> echo_handler(const std::string& request_data) {
    auto req = flatbuffers::GetRoot<echo::EchoRequest>(request_data.data());
    std::cout << "[echo] Received: " << req->message()->str() << std::endl;

    flatbuffers::FlatBufferBuilder builder;
    auto msg = builder.CreateString(req->message()->str());
    auto resp = echo::CreateEchoResponse(builder, msg);
    builder.Finish(resp);
    co_return serialize_fb(builder);
}

Task<std::string> hello_handler(const std::string& request_data) {
    auto req = flatbuffers::GetRoot<echo::HelloRequest>(request_data.data());
    std::cout << "[hello] Received name: " << req->name()->str() << std::endl;

    flatbuffers::FlatBufferBuilder builder;
    auto greeting = builder.CreateString("Hello, " + req->name()->str() + "!");
    auto resp = echo::CreateHelloResponse(builder, greeting);
    builder.Finish(resp);
    co_return serialize_fb(builder);
}

// ============================================================================
// Server-streaming handler: sends N echo responses
// ============================================================================

Task<void> handle_server_stream(const std::string& request_data, writer& w) {
    auto req = flatbuffers::GetRoot<echo::EchoRequest>(request_data.data());
    std::cout << "[server-stream] Request: " << req->message()->str() << std::endl;

    for (int i = 1; i <= 3; ++i) {
        flatbuffers::FlatBufferBuilder builder;
        auto msg = builder.CreateString("Response #" + std::to_string(i) + ": " + req->message()->str());
        auto resp = echo::CreateEchoResponse(builder, msg);
        builder.Finish(resp);

        std::string serialized = serialize_fb(builder);
        std::cout << "[server-stream] Sending response #" << i << std::endl;
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
        auto req = flatbuffers::GetRoot<echo::EchoRequest>(msg->data());
        std::cout << "[client-stream] Received: " << req->message()->str() << std::endl;
        if (!combined.empty()) combined += ", ";
        combined += req->message()->str();
        ++count;
    }

    flatbuffers::FlatBufferBuilder builder;
    auto result = builder.CreateString("Received " + std::to_string(count) + " messages: [" + combined + "]");
    auto resp = echo::CreateEchoResponse(builder, result);
    builder.Finish(resp);

    std::cout << "[client-stream] Final: " << count << " messages" << std::endl;
    co_return serialize_fb(builder);
}

// ============================================================================
// Bidirectional-streaming handler: echoes each message in real-time
// ============================================================================

Task<void> handle_bidi_stream(reader& r, writer& w) {
    std::cout << "[bidi-stream] Started" << std::endl;

    while (auto msg = co_await r.read()) {
        auto req = flatbuffers::GetRoot<echo::EchoRequest>(msg->data());
        std::cout << "[bidi-stream] Received: " << req->message()->str() << std::endl;

        flatbuffers::FlatBufferBuilder builder;
        auto echo_msg = builder.CreateString("Echo: " + req->message()->str());
        auto resp = echo::CreateEchoResponse(builder, echo_msg);
        builder.Finish(resp);

        co_await w.write(serialize_fb(builder));
    }

    co_await w.finish();
}

// ============================================================================
// Main
// ============================================================================

Task<void> main_coroutine(io_context& ctx) {
    frpc::server srv(ctx, 50052);

    // Unary RPCs
    srv.register_method("EchoService", "Echo", echo_handler);
    srv.register_method("GreeterService", "SayHello", hello_handler);

    // Streaming RPCs
    srv.register_server_stream("EchoService", "ServerStream", handle_server_stream);
    srv.register_client_stream("EchoService", "ClientStream", handle_client_stream);
    srv.register_bidi_stream("EchoService", "BidiStream", handle_bidi_stream);

    std::cout << "FRPC Echo Server starting on port 50052..." << std::endl;
    std::cout << "Services (FlatBuffers):" << std::endl;
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
