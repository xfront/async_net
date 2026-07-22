cmake_minimum_required(VERSION 3.20)
project(async_net LANGUAGES CXX)

# Include custom CMake modules
include(cmake/AsyncNetProtobuf.cmake)
include(cmake/AsyncNetFlatBuffers.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional SSL support
option(ASYNC_NET_WITH_SSL "Enable SSL/TLS support" ON)
set(ASYNC_NET_SSL_BACKEND "wolfssl" CACHE STRING "SSL backend: wolfssl, awslc, or libressl")
set_property(CACHE ASYNC_NET_SSL_BACKEND PROPERTY STRINGS wolfssl awslc libressl)

# HTTP/2 support (self-contained, no external dependency)
option(ASYNC_NET_WITH_HTTP2 "Enable HTTP/2 support" ON)

# HTTP/3 support (SSL + ngtcp2 + self-contained QPACK/H3)
option(ASYNC_NET_WITH_HTTP3 "Enable HTTP/3 support (QUIC)" ON)

# gRPC support (HTTP/2 + protobuf + self-contained wire protocol)
option(ASYNC_NET_WITH_GRPC "Enable gRPC support" ON)

# FRPC support (HTTP/2 + FlatBuffers + self-contained wire protocol)
option(ASYNC_NET_WITH_FRPC "Enable FRPC (FlatBuffers RPC) support" ON)

# Compiler-specific coroutine flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
endif()

# Statically link C++ runtime on Windows (avoid MinGW DLL dependencies)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Find SSL backend
set(_VCPKG_HINTS "")
if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND _VCPKG_HINTS
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug")
endif()

set(ASYNC_NET_SSL_FOUND FALSE)
if(ASYNC_NET_WITH_SSL)
    if(ASYNC_NET_SSL_BACKEND STREQUAL "wolfssl")
        find_package(wolfssl CONFIG HINTS ${_VCPKG_HINTS})
        if(wolfssl_FOUND)
            set(ASYNC_NET_SSL_FOUND TRUE)
            message(STATUS "SSL backend: wolfSSL (via vcpkg)")
        else()
            message(STATUS "SSL backend: wolfSSL not found")
        endif()
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "awslc")
        find_package(AWS-LC CONFIG HINTS ${_VCPKG_HINTS})
        if(AWS-LC_FOUND)
            set(ASYNC_NET_SSL_FOUND TRUE)
            message(STATUS "SSL backend: AWS-LC (via vcpkg)")
        else()
            find_package(OpenSSL CONFIG HINTS ${_VCPKG_HINTS})
            if(NOT OpenSSL_FOUND)
                # Fall back to module mode for system OpenSSL
                find_package(OpenSSL)
            endif()
            if(OpenSSL_FOUND)
                set(ASYNC_NET_SSL_FOUND TRUE)
                message(STATUS "SSL backend: AWS-LC/OpenSSL")
            else()
                message(STATUS "SSL backend: AWS-LC not found")
            endif()
        endif()
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "libressl")
        find_package(LibreSSL CONFIG HINTS ${_VCPKG_HINTS})
        if(LibreSSL_FOUND)
            set(ASYNC_NET_SSL_FOUND TRUE)
            message(STATUS "SSL backend: LibreSSL (via vcpkg)")
        else()
            find_package(OpenSSL CONFIG HINTS ${_VCPKG_HINTS})
            if(OpenSSL_FOUND)
                set(ASYNC_NET_SSL_FOUND TRUE)
                message(STATUS "SSL backend: LibreSSL/OpenSSL (via vcpkg)")
            else()
                message(STATUS "SSL backend: LibreSSL not found")
            endif()
        endif()
    else()
        message(WARNING "Unknown SSL backend: ${ASYNC_NET_SSL_BACKEND}")
    endif()
endif()

# Find ngtcp2 (for HTTP/3)
find_package(ngtcp2 CONFIG HINTS ${_VCPKG_HINTS})

# Find protobuf (for gRPC)
set(ASYNC_NET_GRPC_FOUND FALSE)
if(ASYNC_NET_WITH_GRPC)
    find_package(protobuf CONFIG HINTS ${_VCPKG_HINTS})
    if(protobuf_FOUND)
        set(ASYNC_NET_GRPC_FOUND TRUE)
        message(STATUS "gRPC support: enabled (protobuf via vcpkg)")
    else()
        # Try module mode for system protobuf
        find_package(Protobuf)
        if(Protobuf_FOUND)
            set(ASYNC_NET_GRPC_FOUND TRUE)
            message(STATUS "gRPC support: enabled (system protobuf)")
        else()
            message(STATUS "gRPC support: disabled (protobuf not found)")
        endif()
    endif()
endif()

# Library sources - platform specific
set(ASYNC_NET_SOURCES
    src/io/io_context.cpp
    src/net/dns.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND ASYNC_NET_SOURCES
        src/io/epoll_backend.cpp
        src/io/io_uring_backend.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND ASYNC_NET_SOURCES
        src/io/iocp_backend.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    list(APPEND ASYNC_NET_SOURCES
        src/io/kqueue_backend.cpp
    )
endif()

# HTTP sources (always included)
list(APPEND ASYNC_NET_SOURCES
    src/http/http1_codec.cpp
    src/http/server_impl.cpp
    src/http/client_impl.cpp
)

# SSL support
set(ASYNC_NET_HAS_SSL FALSE)
if(ASYNC_NET_WITH_SSL AND ASYNC_NET_SSL_FOUND)
    list(APPEND ASYNC_NET_SOURCES src/io/ssl.cpp src/io/dtls.cpp src/security/aes_gcm.cpp)
    # SSL + DTLS + Crypto backend implementation (one per SSL backend)
    if(ASYNC_NET_SSL_BACKEND STREQUAL "wolfssl")
        list(APPEND ASYNC_NET_SOURCES
                src/security/ssl_wolfssl.cpp
                src/security/dtls_wolfssl.cpp
                src/security/crypto_wolfssl.cpp)
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "awslc" OR ASYNC_NET_SSL_BACKEND STREQUAL "libressl")
        list(APPEND ASYNC_NET_SOURCES
                src/security/ssl_openssl.cpp
                src/security/dtls_openssl.cpp
                src/security/crypto_openssl.cpp)
    endif()
    set(ASYNC_NET_HAS_SSL TRUE)
endif()

# P2P support (requires SSL for DTLS peer connections)
set(ASYNC_NET_HAS_P2P FALSE)
if(ASYNC_NET_HAS_SSL)
    list(APPEND ASYNC_NET_SOURCES
        src/p2p/tracker_server.cpp
        src/p2p/tracker_client.cpp
        src/p2p/peer_connection.cpp
    )
    set(ASYNC_NET_HAS_P2P TRUE)
    message(STATUS "P2P support: enabled")
endif()

# HTTP/2 support (self-contained implementation, no nghttp2 needed)
if(ASYNC_NET_WITH_HTTP2)
    list(APPEND ASYNC_NET_SOURCES
        src/http/http2_session.cpp
        src/http/hpack.cpp
    )
    set(ASYNC_NET_HAS_HTTP2 TRUE)
    message(STATUS "HTTP/2 support: enabled (self-contained)")
else()
    set(ASYNC_NET_HAS_HTTP2 FALSE)
endif()

# HTTP/3 support (SSL backend + ngtcp2 via vcpkg)
# LibreSSL >= 3.9.2 supports QUIC via quictls-compatible API
set(ASYNC_NET_HAS_HTTP3 FALSE)
if(ASYNC_NET_WITH_HTTP3)
    if(ASYNC_NET_SSL_FOUND AND ngtcp2_FOUND)
        list(APPEND ASYNC_NET_SOURCES
            src/http/http3_session.cpp
            src/http/qpack.cpp
        )
        # HTTP/3 QUIC SSL backend implementation
        if(ASYNC_NET_SSL_BACKEND STREQUAL "wolfssl")
            list(APPEND ASYNC_NET_SOURCES src/security/h3_quic_wolfssl.cpp)
        elseif(ASYNC_NET_SSL_BACKEND STREQUAL "awslc" OR ASYNC_NET_SSL_BACKEND STREQUAL "libressl")
            list(APPEND ASYNC_NET_SOURCES src/security/h3_quic_openssl.cpp)
        endif()
        set(ASYNC_NET_HAS_HTTP3 TRUE)
        message(STATUS "HTTP/3 support: enabled (${ASYNC_NET_SSL_BACKEND} + ngtcp2 via vcpkg)")
    else()
        message(STATUS "HTTP/3 support: disabled (SSL backend/ngtcp2 not found)")
    endif()
endif()

# gRPC support (HTTP/2 + wire protocol)
# Note: protobuf is optional - core gRPC just passes raw bytes
set(ASYNC_NET_HAS_GRPC FALSE)
if(ASYNC_NET_WITH_GRPC AND ASYNC_NET_HAS_HTTP2)
    list(APPEND ASYNC_NET_SOURCES
        src/grpc/grpc_wire.cpp
        src/grpc/grpc_server.cpp
        src/grpc/grpc_channel.cpp
        src/grpc/grpc_stream.cpp
    )
    set(ASYNC_NET_HAS_GRPC TRUE)
endif()

# FRPC support (HTTP/2 + FlatBuffers wire protocol)
# FRPC depends on gRPC for shared stream abstractions
set(ASYNC_NET_HAS_FRPC FALSE)
if(ASYNC_NET_WITH_FRPC AND ASYNC_NET_HAS_GRPC)
    list(APPEND ASYNC_NET_SOURCES
        src/frpc/frpc_server.cpp
        src/frpc/frpc_channel.cpp
    )
    set(ASYNC_NET_HAS_FRPC TRUE)
endif()

add_library(async_net ${ASYNC_NET_SOURCES})
target_include_directories(async_net PUBLIC include PRIVATE src)

# gRPC compile definitions and linkage (after library creation)
if(ASYNC_NET_HAS_GRPC)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_GRPC=1)
    
    # Link protobuf if found (optional)
    if(ASYNC_NET_GRPC_FOUND)
        if(TARGET protobuf::libprotobuf)
            target_link_libraries(async_net PUBLIC protobuf::libprotobuf)
            target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_PROTOBUF=1)
        else()
            target_link_libraries(async_net PUBLIC ${Protobuf_LIBRARIES})
            target_include_directories(async_net PUBLIC ${Protobuf_INCLUDE_DIRS})
            target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_PROTOBUF=1)
        endif()
        message(STATUS "gRPC support: enabled (with protobuf)")
    else()
        message(STATUS "gRPC support: enabled (without protobuf - raw bytes only)")
    endif()
endif()

# FRPC compile definitions and linkage (after library creation)
if(ASYNC_NET_HAS_FRPC)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_FRPC=1)
    message(STATUS "FRPC support: enabled (FlatBuffers RPC)")
endif()

# SSL backend linkage
if(ASYNC_NET_HAS_SSL)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_SSL=1)
    if(ASYNC_NET_SSL_BACKEND STREQUAL "wolfssl")
        target_compile_definitions(async_net PUBLIC ASYNC_NET_SSL_WOLFSSL=1)
        target_link_libraries(async_net PUBLIC wolfssl::wolfssl)
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "awslc")
        target_compile_definitions(async_net PUBLIC ASYNC_NET_SSL_AWSLC=1)
        if(TARGET AWS-LC::crypto)
            target_link_libraries(async_net PUBLIC AWS-LC::ssl AWS-LC::crypto)
        else()
            target_link_libraries(async_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        endif()
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "libressl")
        target_compile_definitions(async_net PUBLIC ASYNC_NET_SSL_LIBRESSL=1)
        if(TARGET LibreSSL::TLS)
            target_link_libraries(async_net PUBLIC LibreSSL::TLS LibreSSL::Crypto)
        else()
            target_link_libraries(async_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()
endif()

if(ASYNC_NET_HAS_P2P)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_P2P=1)
endif()

if(ASYNC_NET_HAS_HTTP2)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_HTTP2=1)
endif()

if(ASYNC_NET_HAS_HTTP3)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_HTTP3=1)
    # ngtcp2 crypto library depends on SSL backend
    if(ASYNC_NET_SSL_BACKEND STREQUAL "wolfssl")
        find_library(NGTCP2_CRYPTO_SSL_LIB NAMES ngtcp2_crypto_wolfssl
            PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            NO_DEFAULT_PATH)
    elseif(ASYNC_NET_SSL_BACKEND STREQUAL "awslc" OR ASYNC_NET_SSL_BACKEND STREQUAL "libressl")
        find_library(NGTCP2_CRYPTO_SSL_LIB NAMES ngtcp2_crypto_quictls
            PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            NO_DEFAULT_PATH)
    endif()

    # Link ngtcp2 - try imported target first, then find library directly
    if(TARGET ngtcp2::ngtcp2_static)
        target_link_libraries(async_net PUBLIC ${NGTCP2_CRYPTO_SSL_LIB} ngtcp2::ngtcp2_static)
    elseif(TARGET ngtcp2::ngtcp2)
        target_link_libraries(async_net PUBLIC ${NGTCP2_CRYPTO_SSL_LIB} ngtcp2::ngtcp2)
    else()
        # Fallback: find library directly
        find_library(NGTCP2_LIB NAMES ngtcp2 ngtcp2_static
            PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
            NO_DEFAULT_PATH)
        if(NGTCP2_LIB)
            target_link_libraries(async_net PUBLIC ${NGTCP2_CRYPTO_SSL_LIB} ${NGTCP2_LIB})
        else()
            message(FATAL_ERROR "ngtcp2 library not found")
        endif()
    endif()
endif()

# Platform-specific linkage
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(async_net PUBLIC pthread)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(async_net PUBLIC ws2_32 mswsock)
endif()

# Subdirectories
enable_testing()
add_subdirectory(examples)
add_subdirectory(tests)
add_subdirectory(benchmarks/async_net)
