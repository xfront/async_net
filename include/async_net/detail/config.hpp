#pragma once

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define ASYNC_NET_WINDOWS 1
    #define ASYNC_NET_BACKEND "iocp"
#elif defined(__linux__)
    #define ASYNC_NET_LINUX 1
    #define ASYNC_NET_BACKEND "epoll"
#elif defined(__APPLE__)
    #define ASYNC_NET_MACOS 1
    #define ASYNC_NET_BACKEND "kqueue"
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #define ASYNC_NET_BSD 1
    #define ASYNC_NET_BACKEND "kqueue"
#else
    #error "Unsupported platform"
#endif

// Socket type
#ifdef ASYNC_NET_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "mswsock.lib")
    using socket_t = SOCKET;
    constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/ioctl.h>
    #include <netdb.h>
    using socket_t = int;
    constexpr socket_t invalid_socket = -1;
#endif

#include <cstddef>
#include <cstdint>

namespace async_net {

// Close socket helper
inline void close_socket(socket_t fd) {
#ifdef ASYNC_NET_WINDOWS
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

// Set socket non-blocking
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

// Set socket reuse address
inline int set_reuse_addr(socket_t fd) {
    int opt = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&opt), sizeof(opt));
}

// Last error code
inline int last_error() {
#ifdef ASYNC_NET_WINDOWS
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// Check if error is would-block
inline bool is_would_block() {
#ifdef ASYNC_NET_WINDOWS
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

} // namespace async_net
