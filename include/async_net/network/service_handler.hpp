#pragma once

// ---------------------------------------------------------------------------
// service_handler — Service Handler pattern (C++20 coroutine adaptation)
//
// Base class for connection-oriented services. The lifecycle:
//   1. open(peer_stream)  — called by Acceptor/Connector after connection
//   2. run()              — main coroutine for connection processing
//   3. handle_close()     — cleanup when connection ends
//
// The handler is kept alive by shared_ptr ownership throughout the lifecycle.
// Subclasses implement run() as a coroutine that performs the service logic.
//
// Stream concept (PeerStream) must provide:
//   async_read_some(mutable_buffer) -> Task<ssize_t>
//   async_write_some(const_buffer)  -> Task<ssize_t>
//   is_open() -> bool
//   close()
// Both tcp::socket and ssl::stream satisfy this interface.
// ---------------------------------------------------------------------------

#include <async_net/coroutine/task.hpp>
#include <async_net/io/tcp.hpp>
#include <memory>
#include <optional>

namespace async_net::network {

template<typename PeerStream = tcp::socket>
class service_handler : public std::enable_shared_from_this<service_handler<PeerStream>> {
public:
    using peer_stream_type = PeerStream;

    virtual ~service_handler() = default;

    /// Called by Acceptor or Connector after the connection is established.
    /// Subclasses may override to perform TLS handshake, protocol negotiation, etc.
    virtual void open(PeerStream peer) { peer_.emplace(std::move(peer)); }

    /// Main coroutine — runs the connection processing logic.
    /// Returns when the connection is complete (or failed).
    virtual Task<void> run() = 0;

    /// Called when the connection is closing — override for custom cleanup.
    virtual void handle_close() { if (peer_) peer_->close(); }

    PeerStream& peer() { return *peer_; }
    const PeerStream& peer() const { return *peer_; }

    io_context& get_io_context() { return peer().get_io_context(); }
    bool is_open() const { return peer_.has_value() && peer_->is_open(); }

protected:
    std::optional<PeerStream> peer_;
};

} // namespace async_net::network
