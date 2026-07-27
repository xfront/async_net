#pragma once

// SSL backend policy — compile-time selection.
// This header is a forwarding header for backward compatibility.
// The actual policy declarations live in:
//   include/async_net/io/detail/ssl_policy_fwd.hpp
//
// Including <async_net/io/ssl.hpp> provides:
//   - Policy struct declarations (via ssl_policy_fwd.hpp, when ASYNC_NET_HAS_SSL)
//   - Error code constants: ERR_NONE, ERR_WANT_READ, ERR_WANT_WRITE, ERR_ZERO_RETURN

#include <async_net/io/ssl.hpp>
