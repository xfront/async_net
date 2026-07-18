cmake_minimum_required(VERSION 3.20)
project(async_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional SSL support (wolfSSL)
option(ASYNC_NET_WITH_SSL "Enable SSL/TLS support (requires wolfSSL)" ON)

# HTTP/2 support (self-contained, no external dependency)
option(ASYNC_NET_WITH_HTTP2 "Enable HTTP/2 support" ON)

# HTTP/3 support (wolfSSL + ngtcp2 + self-contained QPACK/H3)
option(ASYNC_NET_WITH_HTTP3 "Enable HTTP/3 support (QUIC)" ON)

# Compiler-specific coroutine flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
endif()

# Statically link C++ runtime on Windows (avoid MinGW DLL dependencies)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Find dependencies via vcpkg
# HINTS ensure packages are found even when vcpkg toolchain doesn't set CMAKE_PREFIX_PATH
set(_VCPKG_HINTS "")
if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND _VCPKG_HINTS
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug")
endif()
find_package(wolfssl CONFIG HINTS ${_VCPKG_HINTS})
find_package(ngtcp2 CONFIG HINTS ${_VCPKG_HINTS})

# Library sources - platform specific
set(ASYNC_NET_SOURCES
    src/io/io_context.cpp
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

# SSL support (wolfSSL via vcpkg)
set(ASYNC_NET_HAS_SSL FALSE)
if(ASYNC_NET_WITH_SSL)
    if(wolfssl_FOUND)
        list(APPEND ASYNC_NET_SOURCES src/io/ssl.cpp src/io/dtls.cpp src/crypto/aes_gcm.cpp)
        set(ASYNC_NET_HAS_SSL TRUE)
        message(STATUS "SSL support: enabled (wolfSSL via vcpkg)")
    else()
        message(STATUS "SSL support: disabled (wolfssl not found)")
    endif()
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

# HTTP/3 support (wolfSSL + ngtcp2 via vcpkg)
set(ASYNC_NET_HAS_HTTP3 FALSE)
if(ASYNC_NET_WITH_HTTP3)
    if(wolfssl_FOUND AND ngtcp2_FOUND)
        list(APPEND ASYNC_NET_SOURCES
            src/http/http3_session.cpp
            src/http/qpack.cpp
        )
        set(ASYNC_NET_HAS_HTTP3 TRUE)
        message(STATUS "HTTP/3 support: enabled (wolfSSL + ngtcp2 via vcpkg)")
    else()
        message(STATUS "HTTP/3 support: disabled (wolfssl/ngtcp2 not found)")
    endif()
endif()

add_library(async_net ${ASYNC_NET_SOURCES})
target_include_directories(async_net PUBLIC include PRIVATE src)

# wolfSSL linkage (used by SSL, H2-over-TLS, and H3)
if(ASYNC_NET_HAS_SSL)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_SSL=1)
    target_link_libraries(async_net PUBLIC wolfssl::wolfssl)
endif()

if(ASYNC_NET_HAS_HTTP2)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_HTTP2=1)
endif()

if(ASYNC_NET_HAS_HTTP3)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_HTTP3=1)
    # ngtcp2_crypto_wolfssl is not exported as a CMake target, link by path
    find_library(NGTCP2_CRYPTO_WOLFSSL_LIB NAMES ngtcp2_crypto_wolfssl
        PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        NO_DEFAULT_PATH)
    target_link_libraries(async_net PUBLIC ${NGTCP2_CRYPTO_WOLFSSL_LIB} ngtcp2::ngtcp2_static)
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
