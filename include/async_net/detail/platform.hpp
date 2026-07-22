#pragma once

// Platform abstraction layer — unified interface for OS-specific APIs
// Include this header instead of <sys/socket.h>, <unistd.h>, <arpa/inet.h>, etc.

#include "config.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>

namespace async_net::platform {

// ============================================================================
// Socket close
// ============================================================================
inline int close_socket(socket_t fd) {
#ifdef ASYNC_NET_WINDOWS
    return ::closesocket(fd);
#else
    return ::close(fd);
#endif
}

// ============================================================================
// Set socket non-blocking
// ============================================================================
inline int set_nonblocking(socket_t fd) {
#ifdef ASYNC_NET_WINDOWS
    u_long mode = 1;
    return ::ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ============================================================================
// setsockopt wrapper (handles const char* vs void* difference)
// ============================================================================
inline int set_socket_option(socket_t fd, int level, int optname,
                             const void* optval, std::size_t optlen) {
#ifdef ASYNC_NET_WINDOWS
    return ::setsockopt(fd, level, optname,
                        static_cast<const char*>(optval),
                        static_cast<int>(optlen));
#else
    return ::setsockopt(fd, level, optname, optval,
                        static_cast<socklen_t>(optlen));
#endif
}

// Convenience: set integer socket option
inline int set_socket_option_int(socket_t fd, int level, int optname, int value) {
    return set_socket_option(fd, level, optname, &value, sizeof(value));
}

// ============================================================================
// Socket reuse address / port
// ============================================================================
inline int set_reuse_addr(socket_t fd) {
    return set_socket_option_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
}

inline int set_reuse_port(socket_t fd) {
#ifdef SO_REUSEPORT
    return set_socket_option_int(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#else
    (void)fd;
    return 0;
#endif
}

// ============================================================================
// Error handling
// ============================================================================
inline int last_error() {
#ifdef ASYNC_NET_WINDOWS
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool is_would_block() {
#ifdef ASYNC_NET_WINDOWS
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

inline bool is_would_block(int err) {
#ifdef ASYNC_NET_WINDOWS
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS;
#endif
}

// ============================================================================
// Socket I/O wrappers
// ============================================================================

// Send on a connected socket (replaces ::write on POSIX, ::send on Windows)
inline auto socket_send(socket_t fd, const void* buf, std::size_t len) {
#ifdef ASYNC_NET_WINDOWS
    return ::send(fd, static_cast<const char*>(buf), static_cast<int>(len), 0);
#else
    return ::write(fd, buf, len);
#endif
}

// Receive from a connected socket (replaces ::read on POSIX, ::recv on Windows)
inline auto socket_recv(socket_t fd, void* buf, std::size_t len) {
#ifdef ASYNC_NET_WINDOWS
    return ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0);
#else
    return ::read(fd, buf, len);
#endif
}

// recvfrom with proper type handling
inline auto socket_recvfrom(socket_t fd, void* buf, std::size_t len, int flags,
                            struct sockaddr* addr, socklen_t* addrlen) {
#ifdef ASYNC_NET_WINDOWS
    return ::recvfrom(fd, static_cast<char*>(buf), static_cast<int>(len), flags, addr, addrlen);
#else
    return ::recvfrom(fd, buf, len, flags, addr, addrlen);
#endif
}

// sendto with proper type handling
inline auto socket_sendto(socket_t fd, const void* buf, std::size_t len, int flags,
                          const struct sockaddr* addr, socklen_t addrlen) {
#ifdef ASYNC_NET_WINDOWS
    return ::sendto(fd, static_cast<const char*>(buf), static_cast<int>(len), flags, addr, static_cast<int>(addrlen));
#else
    return ::sendto(fd, buf, len, flags, addr, addrlen);
#endif
}

// ============================================================================
// Network byte order (available on all platforms via winsock / arpa)
// htonl, htons, ntohl, ntohs are already available from:
//   - Windows: <winsock2.h> (included by config.hpp)
//   - POSIX:   <arpa/inet.h> (included by config.hpp)
// Note: On some platforms (macOS/BSD) these may be macros, so we cannot
// use 'using ::htonl' declarations. They are usable directly as-is.
// ============================================================================

// ============================================================================
// Select wrapper (handles nfds difference on Windows)
// ============================================================================
inline int socket_select(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds,
                         struct timeval* timeout) {
#ifdef ASYNC_NET_WINDOWS
    // Windows ignores nfds parameter
    return ::select(0, rfds, wfds, efds, timeout);
#else
    return ::select(nfds, rfds, wfds, efds, timeout);
#endif
}

// ============================================================================
// Socket creation helpers
// ============================================================================
inline socket_t create_tcp_socket() {
#ifdef ASYNC_NET_WINDOWS
    return ::socket(AF_INET, SOCK_STREAM, 0);
#else
    return ::socket(AF_INET, SOCK_STREAM, 0);
#endif
}

inline socket_t create_udp_socket() {
#ifdef ASYNC_NET_WINDOWS
    return ::socket(AF_INET, SOCK_DGRAM, 0);
#else
    return ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
}

// ============================================================================
// Bind / Connect / Listen / Accept (thin wrappers for type safety)
// ============================================================================
inline int socket_bind(socket_t fd, const struct sockaddr* addr, socklen_t addrlen) {
    return ::bind(fd, addr, addrlen);
}

inline int socket_connect(socket_t fd, const struct sockaddr* addr, socklen_t addrlen) {
    return ::connect(fd, addr, addrlen);
}

inline int socket_listen(socket_t fd, int backlog) {
    return ::listen(fd, backlog);
}

inline socket_t socket_accept(socket_t fd, struct sockaddr* addr, socklen_t* addrlen) {
    return ::accept(fd, addr, addrlen);
}

// ============================================================================
// WSA startup (Windows only, no-op on other platforms)
// ============================================================================
struct wsa_init {
#ifdef ASYNC_NET_WINDOWS
    wsa_init() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~wsa_init() { WSACleanup(); }
#else
    wsa_init() {}
    ~wsa_init() {}
#endif
};

} // namespace async_net::platform
