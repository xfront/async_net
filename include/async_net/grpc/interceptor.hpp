// gRPC Interceptor — pre/post processing hooks for RPC calls
// Interceptors run in order before the actual method handler.
#pragma once

#include <async_net/grpc/types.hpp>
#include <async_net/coroutine/task.hpp>
#include <functional>
#include <string>
#include <vector>

namespace async_net::grpc {

// ============================================================================
// Call Info — information about the current RPC call
// ============================================================================

class call_info {
public:
    call_info(const std::string& svc, const std::string& mtd, const metadata& md)
        : service_(svc), method_(mtd), metadata_(md) {}

    const std::string& service() const { return service_; }
    const std::string& method() const { return method_; }
    const metadata& initial_metadata() const { return metadata_; }

    // Full path: "/Service/Method"
    std::string full_method() const { return "/" + service_ + "/" + method_; }

private:
    std::string service_;
    std::string method_;
    metadata metadata_;
};

// ============================================================================
// Interceptor Function
// ============================================================================

// Interceptor signature:
//   - call_info: information about the call
//   - request: the incoming protobuf request data (can be inspected)
//   - Returns true to proceed, false to reject the call
//   - If false, set response to the error protobuf data to send back
using interceptor_fn = std::function<Task<bool>(
    const call_info& info,
    const std::string& request,
    std::string& error_response,
    status& error_status)>;

// ============================================================================
// Server Interceptor Chain
// ============================================================================

class server_interceptor_chain {
public:
    // Add an interceptor to the chain
    void add(interceptor_fn interceptor) {
        interceptors_.push_back(std::move(interceptor));
    }

    // Run all interceptors in order
    // Returns true if all passed, false if one rejected
    Task<bool> run(const call_info& info, const std::string& request,
                   std::string& error_response, status& error_status) {
        for (auto& interceptor : interceptors_) {
            auto task = interceptor(info, request, error_response, error_status);
            task.resume();
            if (task.done()) {
                bool proceed = task.handle().promise().result();
                if (!proceed) {
                    co_return false;
                }
            }
            // If interceptor didn't complete synchronously, proceed
            // (async interceptors would need more complex handling)
        }
        co_return true;
    }

    bool empty() const { return interceptors_.empty(); }
    size_t size() const { return interceptors_.size(); }

private:
    std::vector<interceptor_fn> interceptors_;
};

} // namespace async_net::grpc
