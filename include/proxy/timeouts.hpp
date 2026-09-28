#pragma once

namespace proxy {
namespace timeouts {

/// Seconds to wait for the client to finish sending HTTP request headers.
inline constexpr int CLIENT_HEADER_TIMEOUT     = 30;

/// Seconds allowed for the TCP handshake to the upstream server.
inline constexpr int UPSTREAM_CONNECT_TIMEOUT  = 10;

/// Seconds to wait for data from the upstream server on HTTP connections.
inline constexpr int UPSTREAM_READ_TIMEOUT     = 60;

/// Seconds without traffic in either direction before closing an HTTPS tunnel.
inline constexpr int HTTPS_TUNNEL_IDLE_TIMEOUT = 120;

/// Maximum blocked write interval, for clients and upstream servers.
inline constexpr int SOCKET_WRITE_TIMEOUT      = 60;

} // namespace timeouts
} // namespace proxy
