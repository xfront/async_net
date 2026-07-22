// FRPC Server implementation — FlatBuffers RPC over HTTP/2
// Supports Unary, Server Streaming, Client Streaming, and Bidirectional Streaming RPCs.

#include <async_net/frpc/server.hpp>
#include <async_net/http/http2_session.hpp>
#include <iostream>
#include <cstring>

#ifdef ASYNC_NET_HAS_SSL
#include <async_net/net/ssl.hpp>
#endif

namespace async_net::frpc {

// Helper: properly invoke writer::finish() which is a coroutine
static void finish_writer(writer& w, status_code code = status_code::ok) {
    auto ft = new Task<void>(w.finish(code));
    ft->resume();
    delete ft;
}

// ============================================================================
// Constructor
// ============================================================================

server::server(io_context& ctx, uint16_t port, const char* addr)
    : ctx_(ctx)
    , acceptor_(ctx, port, addr)
    , port_(port)
{
}

// ============================================================================
// Register FRPC methods
// ============================================================================

void server::register_method(const std::string& service, const std::string& method, method_handler handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::unary;
    e.has_context = false;
    e.unary = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (unary): " << path << std::endl;
}

void server::register_method(const std::string& service, const std::string& method, method_handler_with_context handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::unary;
    e.has_context = true;
    e.unary_ctx = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (unary+ctx): " << path << std::endl;
}

void server::register_server_stream(const std::string& service, const std::string& method, server_stream_handler handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::server_stream;
    e.has_context = false;
    e.srv_stream = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (server-stream): " << path << std::endl;
}

void server::register_server_stream(const std::string& service, const std::string& method, server_stream_handler_with_context handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::server_stream;
    e.has_context = true;
    e.srv_stream_ctx = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (server-stream+ctx): " << path << std::endl;
}

void server::register_client_stream(const std::string& service, const std::string& method, client_stream_handler handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::client_stream;
    e.has_context = false;
    e.cli_stream = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (client-stream): " << path << std::endl;
}

void server::register_client_stream(const std::string& service, const std::string& method, client_stream_handler_with_context handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::client_stream;
    e.has_context = true;
    e.cli_stream_ctx = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (client-stream+ctx): " << path << std::endl;
}

void server::register_bidi_stream(const std::string& service, const std::string& method, bidi_stream_handler handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::bidi_stream;
    e.has_context = false;
    e.bidi = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (bidi-stream): " << path << std::endl;
}

void server::register_bidi_stream(const std::string& service, const std::string& method, bidi_stream_handler_with_context handler) {
    std::string path = make_path(service, method);
    method_entry e;
    e.type = handler_type::bidi_stream;
    e.has_context = true;
    e.bidi_ctx = std::move(handler);
    methods_[path] = std::move(e);
    std::cout << "[frpc] Registered method (bidi-stream+ctx): " << path << std::endl;
}

// ============================================================================
// Add interceptor
// ============================================================================

void server::add_interceptor(grpc::interceptor_fn interceptor) {
    interceptors_.add(std::move(interceptor));
}

// ============================================================================
// Stop the server
// ============================================================================

void server::stop() {
    running_ = false;
    acceptor_.close();
}

// ============================================================================
// Dispatch a FRPC unary request
// ============================================================================

Task<std::pair<status, std::string>> server::dispatch(const std::string& path, const std::string& data) {
    call_context ctx;
    return dispatch(path, data, ctx);
}

Task<std::pair<status, std::string>> server::dispatch(const std::string& path, const std::string& data, call_context& ctx) {
    auto it = methods_.find(path);
    if (it == methods_.end()) {
        std::cerr << "[frpc] Method not found: " << path << std::endl;
        co_return std::make_pair(status{status_code::unimplemented, "Method not found"}, std::string{});
    }

    if (it->second.type != handler_type::unary) {
        co_return std::make_pair(status{status_code::unimplemented, "Method is not a unary RPC"}, std::string{});
    }

    // Run interceptors
    if (!interceptors_.empty()) {
        auto [svc, mtd] = parse_path(path);
        grpc::call_info info(svc, mtd, ctx.initial_metadata);
        std::string error_response;
        status error_status;
        auto chain_task = interceptors_.run(info, data, error_response, error_status);
        chain_task.resume();
        if (chain_task.done()) {
            bool proceed = chain_task.handle().promise().result();
            if (!proceed) {
                co_return std::make_pair(error_status, std::move(error_response));
            }
        }
    }

    try {
        Task<std::string> task = it->second.has_context
            ? it->second.unary_ctx(data, ctx)
            : it->second.unary(data);
        task.resume();
        if (task.done()) {
            std::string response_data = task.handle().promise().result();
            co_return std::make_pair(status{status_code::ok, ""}, std::move(response_data));
        }
        co_return std::make_pair(status{status_code::internal, "Handler did not complete synchronously"}, std::string{});
    } catch (const std::exception& e) {
        std::cerr << "[frpc] Handler exception: " << e.what() << std::endl;
        co_return std::make_pair(status{status_code::internal, e.what()}, std::string{});
    }
}

// ============================================================================
// Handle a plain HTTP/2 connection
// ============================================================================

Task<void> server::handle_connection(tcp::socket sock) {
    using namespace async_net::http;

    http2_session session(http2_session::mode::server);

    // Normal request handler for unary RPCs
    session.set_request_handler([this](const request& req) -> response {
        auto ct = req.hdrs.get("content-type");
        if (!ct || ct->find("application/grpc") != 0) {
            response resp;
            resp.status = http::status_code(415);
            resp.bd = http::body("Not a gRPC/FRPC request");
            return resp;
        }

        if (req.method != http::method::POST) {
            response resp;
            resp.status = http::status_code(405);
            resp.bd = http::body("RPC requires POST method");
            return resp;
        }

        auto frame_data = decode_grpc_message(req.bd.data());
        if (!frame_data) {
            response resp;
            resp.status = http::status_code::ok();
            resp.hdrs.set("content-type", frpc_constants::content_type);
            resp.trailers.set(frpc_constants::grpc_status, std::to_string(static_cast<int>(status_code::internal)));
            resp.trailers.set(frpc_constants::grpc_message, "Invalid RPC frame");
            return resp;
        }

        call_context ctx;
        ctx.initial_metadata = call_context::extract_from_headers(req.hdrs);

        auto task = dispatch(req.path, *frame_data, ctx);
        task.resume();
        
        status rpc_status{status_code::internal, "Handler did not complete"};
        std::string response_data;
        
        if (task.done()) {
            auto [st, data] = task.handle().promise().result();
            rpc_status = st;
            response_data = std::move(data);
        }

        response resp;
        resp.status = http::status_code::ok();
        resp.hdrs.set("content-type", frpc_constants::content_type);
        resp.hdrs.set("grpc-encoding", frpc_constants::grpc_encoding);

        if (!response_data.empty()) {
            resp.bd = http::body(encode_grpc_message(response_data));
        }

        resp.trailers.set(frpc_constants::grpc_status, std::to_string(static_cast<int>(rpc_status.code)));
        if (!rpc_status.message.empty()) {
            resp.trailers.set(frpc_constants::grpc_message, rpc_status.message);
        }

        for (auto& [k, v] : ctx.response_metadata) {
            resp.trailers.set(k, v);
        }

        return resp;
    });

    // Request start handler — detects streaming methods
    session.set_request_start_handler([this, &session](int32_t stream_id, const request& req, bool end_stream) -> bool {
        auto ct = req.hdrs.get("content-type");
        if (!ct || ct->find("application/grpc") != 0) {
            return false;
        }

        auto path_it = methods_.find(req.path);
        if (path_it == methods_.end() || path_it->second.type == handler_type::unary) {
            return false;
        }

        auto& entry = path_it->second;

        http::response resp;
        resp.status = http::status_code::ok();
        resp.hdrs.set("content-type", frpc_constants::content_type);
        resp.hdrs.set("grpc-encoding", frpc_constants::grpc_encoding);
        session.submit_response_headers(stream_id, resp);

        if (entry.type == handler_type::server_stream) {
            auto buffer = std::make_shared<std::string>();
            auto wrt = std::make_shared<writer>(&session, stream_id);
            auto handler_called = std::make_shared<bool>(false);

            session.set_data_callback(stream_id,
                [buffer, wrt, &entry, handler_called, &session, stream_id](int32_t, const uint8_t* data, size_t len, bool es) {
                    buffer->append(reinterpret_cast<const char*>(data), len);
                    if (es && !*handler_called) {
                        *handler_called = true;
                        auto frame_data = decode_grpc_message(*buffer);
                        std::string request_data = frame_data.value_or("");
                        auto task = new Task<void>(entry.srv_stream(request_data, *wrt));
                        task->resume();
                        if (task->done()) {
                            if (!wrt->is_finished()) finish_writer(*wrt);
                            delete task;
                        }
                    }
                });

            if (end_stream && !*handler_called) {
                *handler_called = true;
                auto task = new Task<void>(entry.srv_stream("", *wrt));
                task->resume();
                if (task->done()) {
                    if (!wrt->is_finished()) finish_writer(*wrt);
                    delete task;
                }
            }
            return true;
        }

        // Client-streaming / bidi: set up reader + deframer
        auto rdr = std::make_shared<reader>();
        auto wrt = std::make_shared<writer>(&session, stream_id);
        auto deframer = std::make_shared<stream_deframer>(rdr);

        if (entry.type == handler_type::client_stream) {
            auto handler_launched = std::make_shared<bool>(false);
            auto entry_ptr = &entry;

            session.set_data_callback(stream_id,
                [deframer, wrt, entry_ptr, handler_launched, &session, stream_id](int32_t, const uint8_t* data, size_t len, bool es) {
                    deframer->feed(data, len);
                    if (es) {
                        deframer->complete();
                        if (!*handler_launched) {
                            *handler_launched = true;
                            auto task = new Task<std::string>(entry_ptr->cli_stream(*deframer->get_reader()));
                            task->resume();
                            if (task->done()) {
                                std::string result = task->handle().promise().result();
                                session.submit_data(stream_id, encode_grpc_message(result), false);
                                finish_writer(*wrt);
                                delete task;
                            }
                        }
                    }
                });

            if (end_stream) {
                deframer->complete();
                auto task = new Task<std::string>(entry.cli_stream(*rdr));
                task->resume();
                if (task->done()) {
                    std::string result = task->handle().promise().result();
                    session.submit_data(stream_id, encode_grpc_message(result), false);
                    finish_writer(*wrt);
                    delete task;
                }
            }
        } else {
            // Bidi
            auto handler_launched = std::make_shared<bool>(false);
            auto entry_ptr = &entry;

            session.set_data_callback(stream_id,
                [deframer, wrt, entry_ptr, handler_launched](int32_t, const uint8_t* data, size_t len, bool es) {
                    deframer->feed(data, len);
                    if (es) {
                        deframer->complete();
                        if (!*handler_launched) {
                            *handler_launched = true;
                            auto task = new Task<void>(entry_ptr->bidi(*deframer->get_reader(), *wrt));
                            task->resume();
                            if (task->done()) {
                                if (!wrt->is_finished()) finish_writer(*wrt);
                                delete task;
                            }
                        }
                    }
                });

            if (end_stream) {
                deframer->complete();
                auto task = new Task<void>(entry.bidi(*rdr, *wrt));
                task->resume();
                if (task->done()) {
                    if (!wrt->is_finished()) finish_writer(*wrt);
                    delete task;
                }
            }
        }

        return true;
    });

    auto send_fn = [&sock](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await sock.async_write_some(const_buffer(data, len));
    };

    co_await session.flush(send_fn);

    // Read client connection preface
    {
        static constexpr const char* PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        static constexpr size_t PREFACE_LEN = 24;
        char preface_buf[256];
        size_t preface_read = 0;
        
        while (preface_read < PREFACE_LEN) {
            auto n = co_await sock.async_read_some(
                mutable_buffer(preface_buf + preface_read, sizeof(preface_buf) - preface_read));
            if (n <= 0) break;
            preface_read += static_cast<size_t>(n);
        }
        
        if (preface_read < PREFACE_LEN ||
            std::memcmp(preface_buf, PREFACE, PREFACE_LEN) != 0) {
            std::cerr << "[frpc] Invalid client connection preface" << std::endl;
            sock.close();
            co_return;
        }
        
        if (preface_read > PREFACE_LEN) {
            session.feed(reinterpret_cast<const uint8_t*>(preface_buf + PREFACE_LEN),
                         preface_read - PREFACE_LEN);
        }
    }

    co_await session.flush(send_fn);

    // Read loop
    char read_buf[16384];
    while (session.is_alive() && running_) {
        auto n = co_await sock.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) break;

        auto consumed = session.feed(reinterpret_cast<const uint8_t*>(read_buf),
                                      static_cast<size_t>(n));
        if (consumed < 0) break;

        bool ok = co_await session.flush(send_fn);
        if (!ok) break;
    }

    sock.close();
}

// ============================================================================
// Start serving (plain HTTP/2)
// ============================================================================

Task<void> server::serve() {
    std::cout << "[frpc] Server listening on port " << port_ << std::endl;

    while (running_) {
        auto sock = co_await acceptor_.async_accept();
        if (!sock.is_open()) {
            if (running_) {
                std::cerr << "[frpc] Accept failed" << std::endl;
            }
            continue;
        }

        auto* task = new Task<void>(handle_connection(std::move(sock)));
        task->resume();
        if (task->done()) {
            delete task;
        }
    }
}

#ifdef ASYNC_NET_HAS_SSL
// ============================================================================
// Handle a TLS connection
// ============================================================================

Task<void> server::handle_tls_connection(tcp::socket sock, ssl::context& ssl_ctx) {
    using namespace async_net::http;

    ssl::stream ssl_stream(sock, ssl_ctx, true);
    auto hs = co_await ssl_stream.async_handshake();
    if (hs <= 0) {
        sock.close();
        co_return;
    }

    http2_session session(http2_session::mode::server);

    // Same handlers as plain connection (abbreviated — same logic)
    session.set_request_handler([this](const request& req) -> response {
        auto ct = req.hdrs.get("content-type");
        if (!ct || ct->find("application/grpc") != 0) {
            response resp;
            resp.status = http::status_code(415);
            resp.bd = http::body("Not a gRPC/FRPC request");
            return resp;
        }

        if (req.method != http::method::POST) {
            response resp;
            resp.status = http::status_code(405);
            resp.bd = http::body("RPC requires POST method");
            return resp;
        }

        auto frame_data = decode_grpc_message(req.bd.data());
        if (!frame_data) {
            response resp;
            resp.status = http::status_code::ok();
            resp.hdrs.set("content-type", frpc_constants::content_type);
            resp.trailers.set(frpc_constants::grpc_status, std::to_string(static_cast<int>(status_code::internal)));
            resp.trailers.set(frpc_constants::grpc_message, "Invalid RPC frame");
            return resp;
        }

        call_context ctx;
        ctx.initial_metadata = call_context::extract_from_headers(req.hdrs);

        auto task = dispatch(req.path, *frame_data, ctx);
        task.resume();
        
        status rpc_status{status_code::internal, "Handler did not complete"};
        std::string response_data;
        
        if (task.done()) {
            auto [st, data] = task.handle().promise().result();
            rpc_status = st;
            response_data = std::move(data);
        }

        response resp;
        resp.status = http::status_code::ok();
        resp.hdrs.set("content-type", frpc_constants::content_type);
        resp.hdrs.set("grpc-encoding", frpc_constants::grpc_encoding);

        if (!response_data.empty()) {
            resp.bd = http::body(encode_grpc_message(response_data));
        }

        resp.trailers.set(frpc_constants::grpc_status, std::to_string(static_cast<int>(rpc_status.code)));
        if (!rpc_status.message.empty()) {
            resp.trailers.set(frpc_constants::grpc_message, rpc_status.message);
        }

        for (auto& [k, v] : ctx.response_metadata) {
            resp.trailers.set(k, v);
        }

        return resp;
    });

    // Request start handler (same as plain connection)
    session.set_request_start_handler([this, &session](int32_t stream_id, const request& req, bool end_stream) -> bool {
        auto ct = req.hdrs.get("content-type");
        if (!ct || ct->find("application/grpc") != 0) return false;

        auto path_it = methods_.find(req.path);
        if (path_it == methods_.end() || path_it->second.type == handler_type::unary) return false;

        auto& entry = path_it->second;

        http::response resp;
        resp.status = http::status_code::ok();
        resp.hdrs.set("content-type", frpc_constants::content_type);
        resp.hdrs.set("grpc-encoding", frpc_constants::grpc_encoding);
        session.submit_response_headers(stream_id, resp);

        if (entry.type == handler_type::server_stream) {
            auto buffer = std::make_shared<std::string>();
            auto wrt = std::make_shared<writer>(&session, stream_id);
            auto handler_called = std::make_shared<bool>(false);

            session.set_data_callback(stream_id,
                [buffer, wrt, &entry, handler_called, &session, stream_id](int32_t, const uint8_t* data, size_t len, bool es) {
                    buffer->append(reinterpret_cast<const char*>(data), len);
                    if (es && !*handler_called) {
                        *handler_called = true;
                        auto frame_data = decode_grpc_message(*buffer);
                        std::string request_data = frame_data.value_or("");
                        auto task = new Task<void>(entry.srv_stream(request_data, *wrt));
                        task->resume();
                        if (task->done()) {
                            if (!wrt->is_finished()) finish_writer(*wrt);
                            delete task;
                        }
                    }
                });

            if (end_stream && !*handler_called) {
                *handler_called = true;
                auto task = new Task<void>(entry.srv_stream("", *wrt));
                task->resume();
                if (task->done()) {
                    if (!wrt->is_finished()) finish_writer(*wrt);
                    delete task;
                }
            }
            return true;
        }

        auto rdr = std::make_shared<reader>();
        auto wrt = std::make_shared<writer>(&session, stream_id);
        auto deframer = std::make_shared<stream_deframer>(rdr);

        if (entry.type == handler_type::client_stream) {
            auto handler_launched = std::make_shared<bool>(false);
            auto entry_ptr = &entry;

            session.set_data_callback(stream_id,
                [deframer, wrt, entry_ptr, handler_launched, &session, stream_id](int32_t, const uint8_t* data, size_t len, bool es) {
                    deframer->feed(data, len);
                    if (es) {
                        deframer->complete();
                        if (!*handler_launched) {
                            *handler_launched = true;
                            auto task = new Task<std::string>(entry_ptr->cli_stream(*deframer->get_reader()));
                            task->resume();
                            if (task->done()) {
                                std::string result = task->handle().promise().result();
                                session.submit_data(stream_id, encode_grpc_message(result), false);
                                finish_writer(*wrt);
                                delete task;
                            }
                        }
                    }
                });

            if (end_stream) {
                deframer->complete();
                auto task = new Task<std::string>(entry.cli_stream(*rdr));
                task->resume();
                if (task->done()) {
                    std::string result = task->handle().promise().result();
                    session.submit_data(stream_id, encode_grpc_message(result), false);
                    finish_writer(*wrt);
                    delete task;
                }
            }
        } else {
            auto handler_launched = std::make_shared<bool>(false);
            auto entry_ptr = &entry;

            session.set_data_callback(stream_id,
                [deframer, wrt, entry_ptr, handler_launched](int32_t, const uint8_t* data, size_t len, bool es) {
                    deframer->feed(data, len);
                    if (es) {
                        deframer->complete();
                        if (!*handler_launched) {
                            *handler_launched = true;
                            auto task = new Task<void>(entry_ptr->bidi(*deframer->get_reader(), *wrt));
                            task->resume();
                            if (task->done()) {
                                if (!wrt->is_finished()) finish_writer(*wrt);
                                delete task;
                            }
                        }
                    }
                });

            if (end_stream) {
                deframer->complete();
                auto task = new Task<void>(entry.bidi(*rdr, *wrt));
                task->resume();
                if (task->done()) {
                    if (!wrt->is_finished()) finish_writer(*wrt);
                    delete task;
                }
            }
        }

        return true;
    });

    auto send_fn = [&ssl_stream](const uint8_t* data, size_t len) -> Task<ssize_t> {
        co_return co_await ssl_stream.async_write_some(const_buffer(data, len));
    };

    co_await session.flush(send_fn);

    // Read client preface
    {
        static constexpr const char* PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        static constexpr size_t PREFACE_LEN = 24;
        char preface_buf[256];
        size_t preface_read = 0;
        
        while (preface_read < PREFACE_LEN) {
            auto n = co_await ssl_stream.async_read_some(
                mutable_buffer(preface_buf + preface_read, sizeof(preface_buf) - preface_read));
            if (n <= 0) break;
            preface_read += static_cast<size_t>(n);
        }
        
        if (preface_read < PREFACE_LEN ||
            std::memcmp(preface_buf, PREFACE, PREFACE_LEN) != 0) {
            std::cerr << "[frpc] Invalid client connection preface" << std::endl;
            co_await ssl_stream.async_shutdown();
            sock.close();
            co_return;
        }
        
        if (preface_read > PREFACE_LEN) {
            session.feed(reinterpret_cast<const uint8_t*>(preface_buf + PREFACE_LEN),
                         preface_read - PREFACE_LEN);
        }
    }

    co_await session.flush(send_fn);

    char read_buf[16384];
    while (session.is_alive() && running_) {
        auto n = co_await ssl_stream.async_read_some(mutable_buffer(read_buf, sizeof(read_buf)));
        if (n <= 0) break;

        auto consumed = session.feed(reinterpret_cast<const uint8_t*>(read_buf),
                                      static_cast<size_t>(n));
        if (consumed < 0) break;

        bool ok = co_await session.flush(send_fn);
        if (!ok) break;
    }

    co_await ssl_stream.async_shutdown();
}

// ============================================================================
// Start serving with TLS
// ============================================================================

Task<void> server::serve_tls(ssl::context& ssl_ctx) {
    std::cout << "[frpc] Server listening on port " << port_ << " (TLS)" << std::endl;

    while (running_) {
        auto sock = co_await acceptor_.async_accept();
        if (!sock.is_open()) {
            if (running_) {
                std::cerr << "[frpc] Accept failed" << std::endl;
            }
            continue;
        }

        auto* task = new Task<void>(handle_tls_connection(std::move(sock), ssl_ctx));
        task->resume();
        if (task->done()) {
            delete task;
        }
    }
}
#endif

} // namespace async_net::frpc
