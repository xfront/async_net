// gRPC Echo Client — demonstrates unary, server-streaming, client-streaming,
// and bidi-streaming RPCs with Protocol Buffers serialization.
//
// Message definitions: examples/grpc/echo.proto (generated via protoc)

#include <async_net/grpc/channel.hpp>
#include <async_net/io/io_context.hpp>
#include "echo.pb.h"
#include <iostream>
#include <string>

using namespace async_net;
using namespace async_net::grpc;

Task<void> main_coroutine(io_context& ctx) {
    channel ch(ctx, "127.0.0.1", 50051);

    std::cout << "Connecting to gRPC server..." << std::endl;
    bool connected = co_await ch.connect();
    if (!connected) {
        std::cerr << "Failed to connect!" << std::endl;
        co_return;
    }
    std::cout << "Connected!" << std::endl;

    // Test 1: Unary — EchoService.Echo
    {
        std::cout << "\n--- Test 1: EchoService/Echo (unary) ---" << std::endl;

        echo::EchoRequest req;
        req.set_message("Hello, gRPC!");
        std::cout << "Request: " << req.message() << std::endl;

        std::string serialized;
        req.SerializeToString(&serialized);

        auto [status, response_data] = co_await ch.unary_call("EchoService", "Echo", serialized);

        std::cout << "Status: " << to_string(status.code);
        if (!status.message.empty()) std::cout << " (" << status.message << ")";
        std::cout << std::endl;

        if (status.ok()) {
            echo::EchoResponse resp;
            resp.ParseFromString(response_data);
            std::cout << "Response: " << resp.message() << std::endl;
        }
    }

    // Test 2: Unary — GreeterService.SayHello
    {
        std::cout << "\n--- Test 2: GreeterService/SayHello (unary) ---" << std::endl;

        echo::HelloRequest req;
        req.set_name("World");
        std::cout << "Request name: " << req.name() << std::endl;

        std::string serialized;
        req.SerializeToString(&serialized);

        auto [status, response_data] = co_await ch.unary_call("GreeterService", "SayHello", serialized);

        std::cout << "Status: " << to_string(status.code);
        if (!status.message.empty()) std::cout << " (" << status.message << ")";
        std::cout << std::endl;

        if (status.ok()) {
            echo::HelloResponse resp;
            resp.ParseFromString(response_data);
            std::cout << "Response: " << resp.greeting() << std::endl;
        }
    }

    // Test 3: Server-streaming — EchoService.ServerStream
    {
        std::cout << "\n--- Test 3: EchoService/ServerStream (server-streaming) ---" << std::endl;

        echo::EchoRequest req;
        req.set_message("StreamTest");
        std::cout << "Request: " << req.message() << std::endl;

        std::string serialized;
        req.SerializeToString(&serialized);

        int msg_count = 0;
        auto status = co_await ch.server_stream_call(
            "EchoService", "ServerStream", serialized,
            [&msg_count](const std::string& msg_data) {
                echo::EchoResponse resp;
                resp.ParseFromString(msg_data);
                std::cout << "  Received message #" << ++msg_count
                          << ": " << resp.message() << std::endl;
            });

        std::cout << "Status: " << to_string(status.code);
        if (!status.message.empty()) std::cout << " (" << status.message << ")";
        std::cout << std::endl;
        std::cout << "Total messages received: " << msg_count << std::endl;
    }

    // Test 4: Client-streaming — EchoService.ClientStream
    {
        std::cout << "\n--- Test 4: EchoService/ClientStream (client-streaming) ---" << std::endl;

        auto state = co_await ch.client_stream_call("EchoService", "ClientStream");
        if (!state.wrt) {
            std::cerr << "Failed to start client stream" << std::endl;
        } else {
            // Send multiple messages
            const std::string names[] = {"Message1", "Message2", "Message3"};
            for (auto& name : names) {
                echo::EchoRequest req;
                req.set_message(name);
                std::string serialized;
                req.SerializeToString(&serialized);
                co_await state.wrt->write(serialized);
                std::cout << "  Sent: " << name << std::endl;
            }

            // Finish the stream
            co_await state.wrt->finish();
            std::cout << "  Stream finished" << std::endl;

            // Wait for response
            co_await PromiseAwaiter{state.promise};

            auto grpc_status = status{status_code::ok, ""};
            auto status_hdr = state.promise->resp.hdrs.get("grpc-status");
            if (status_hdr) {
                int code = 0;
                std::from_chars(status_hdr->data(), status_hdr->data() + status_hdr->size(), code);
                grpc_status.code = static_cast<status_code>(code);
            }

            std::cout << "Status: " << to_string(grpc_status.code) << std::endl;

            // Decode response body
            auto response_data = decode_grpc_message(state.promise->resp.bd.data());
            if (response_data) {
                echo::EchoResponse resp;
                resp.ParseFromString(*response_data);
                std::cout << "Response: " << resp.message() << std::endl;
            }
        }
    }

    // Test 5: Bidirectional-streaming — EchoService.BidiStream
    {
        std::cout << "\n--- Test 5: EchoService/BidiStream (bidirectional) ---" << std::endl;

        auto state = co_await ch.bidi_stream_call("EchoService", "BidiStream");
        if (!state.rdr || !state.wrt) {
            std::cerr << "Failed to start bidi stream" << std::endl;
        } else {
            // Send messages
            const std::string names[] = {"Bidi1", "Bidi2"};
            for (auto& name : names) {
                echo::EchoRequest req;
                req.set_message(name);
                std::string serialized;
                req.SerializeToString(&serialized);
                co_await state.wrt->write(serialized);
                std::cout << "  Sent: " << name << std::endl;
            }

            // Finish writing
            co_await state.wrt->finish();
            std::cout << "  Write stream finished" << std::endl;

            // Read all echoes
            int count = 0;
            while (auto msg = co_await state.rdr->read()) {
                echo::EchoResponse resp;
                resp.ParseFromString(*msg);
                std::cout << "  Received echo #" << ++count
                          << ": " << resp.message() << std::endl;
            }
            std::cout << "Total echoes received: " << count << std::endl;
        }
    }

    // Test 6: Non-existent method
    {
        std::cout << "\n--- Test 6: NonExistent/Method ---" << std::endl;

        echo::EchoRequest req;
        req.set_message("test");
        std::string serialized;
        req.SerializeToString(&serialized);

        auto [status, response] = co_await ch.unary_call("NonExistent", "Method", serialized);

        std::cout << "Status: " << to_string(status.code);
        if (!status.message.empty()) std::cout << " (" << status.message << ")";
        std::cout << std::endl;
    }

    ch.close();
    std::cout << "\nAll tests completed!" << std::endl;

    ctx.stop();
}

int main() {
    io_context ctx;

    auto task = main_coroutine(ctx);
    task.resume();

    ctx.run();

    return 0;
}
