cmake_minimum_required(VERSION 3.20)
project(async_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional SSL support
option(ASYNC_NET_WITH_SSL "Enable SSL/TLS support (requires OpenSSL)" ON)

# Compiler-specific coroutine flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
endif()

# Statically link C++ runtime on Windows (avoid MinGW DLL dependencies)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

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

# Optional SSL support
if(ASYNC_NET_WITH_SSL)
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        list(APPEND ASYNC_NET_SOURCES src/io/ssl.cpp)
        message(STATUS "SSL support: enabled (OpenSSL ${OPENSSL_VERSION})")
    else()
        message(STATUS "SSL support: disabled (OpenSSL not found)")
    endif()
endif()

add_library(async_net ${ASYNC_NET_SOURCES})
target_include_directories(async_net PUBLIC include PRIVATE src)

if(ASYNC_NET_WITH_SSL AND OPENSSL_FOUND)
    target_compile_definitions(async_net PUBLIC ASYNC_NET_HAS_SSL=1)
    target_link_libraries(async_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Platform-specific linkage
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(async_net PUBLIC pthread)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(async_net PUBLIC ws2_32 mswsock)
    if(ASYNC_NET_WITH_SSL AND OPENSSL_FOUND)
        target_link_libraries(async_net PUBLIC crypt32 advapi32 user32)
    endif()
endif()

# Examples
add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE async_net)

add_executable(echo_client examples/echo_client.cpp)
target_link_libraries(echo_client PRIVATE async_net)

# Linux-only examples (io_uring)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(echo_server_iouring examples/echo_server_iouring.cpp)
    target_link_libraries(echo_server_iouring PRIVATE async_net)
    target_include_directories(echo_server_iouring PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

add_executable(multicast_receiver examples/multicast_receiver.cpp)
target_link_libraries(multicast_receiver PRIVATE async_net)

add_executable(multicast_sender examples/multicast_sender.cpp)
target_link_libraries(multicast_sender PRIVATE async_net)

add_executable(broadcast_sender examples/broadcast_sender.cpp)
target_link_libraries(broadcast_sender PRIVATE async_net)

add_executable(broadcast_receiver examples/broadcast_receiver.cpp)
target_link_libraries(broadcast_receiver PRIVATE async_net)

# Tests
enable_testing()
add_executable(test_task tests/test_task.cpp)
target_link_libraries(test_task PRIVATE async_net)
add_test(NAME test_task COMMAND test_task)

# SSL examples (only when SSL is enabled)
if(ASYNC_NET_HAS_SSL OR (ASYNC_NET_WITH_SSL AND OPENSSL_FOUND))
    add_executable(ssl_echo_server examples/ssl_echo_server.cpp)
    target_link_libraries(ssl_echo_server PRIVATE async_net)

    add_executable(ssl_echo_client examples/ssl_echo_client.cpp)
    target_link_libraries(ssl_echo_client PRIVATE async_net)
endif()
