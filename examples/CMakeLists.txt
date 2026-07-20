# Basic examples
add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE async_net)

add_executable(echo_client echo_client.cpp)
target_link_libraries(echo_client PRIVATE async_net)

# Linux-only examples (io_uring)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(echo_server_iouring echo_server_iouring.cpp)
    target_link_libraries(echo_server_iouring PRIVATE async_net)
    target_include_directories(echo_server_iouring PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# UDP multicast / broadcast
add_executable(multicast_receiver multicast_receiver.cpp)
target_link_libraries(multicast_receiver PRIVATE async_net)

add_executable(multicast_sender multicast_sender.cpp)
target_link_libraries(multicast_sender PRIVATE async_net)

add_executable(broadcast_sender broadcast_sender.cpp)
target_link_libraries(broadcast_sender PRIVATE async_net)

add_executable(broadcast_receiver broadcast_receiver.cpp)
target_link_libraries(broadcast_receiver PRIVATE async_net)

# SSL examples
if(ASYNC_NET_HAS_SSL)
    add_executable(ssl_echo_server ssl_echo_server.cpp)
    target_link_libraries(ssl_echo_server PRIVATE async_net)

    add_executable(ssl_echo_client ssl_echo_client.cpp)
    target_link_libraries(ssl_echo_client PRIVATE async_net)

    # DTLS multicast (secure multicast with DTLS key exchange + AES-GCM data)
    add_executable(dtls_multicast_sender dtls_multicast_sender.cpp)
    target_link_libraries(dtls_multicast_sender PRIVATE async_net)

    add_executable(dtls_multicast_receiver dtls_multicast_receiver.cpp)
    target_link_libraries(dtls_multicast_receiver PRIVATE async_net)

    # P2P examples
    if(ASYNC_NET_HAS_P2P)
        add_executable(p2p_tracker p2p_tracker.cpp)
        target_link_libraries(p2p_tracker PRIVATE async_net)

        add_executable(p2p_chat p2p_chat.cpp)
        target_link_libraries(p2p_chat PRIVATE async_net)
    endif()
endif()

# HTTP examples
add_executable(http_server http_server.cpp)
target_link_libraries(http_server PRIVATE async_net)

add_executable(http_client http_client.cpp)
target_link_libraries(http_client PRIVATE async_net)

# H2 server (HTTP/2 + SSL)
if(ASYNC_NET_HAS_HTTP2 AND ASYNC_NET_HAS_SSL)
    add_executable(h2_server h2_server.cpp)
    target_link_libraries(h2_server PRIVATE async_net)
endif()

# H3 server (HTTP/3 + QUIC)
if(ASYNC_NET_HAS_HTTP3)
    add_executable(h3_server h3_server.cpp)
    target_link_libraries(h3_server PRIVATE async_net)
endif()

# Multi-protocol HTTP server (H1 + H2 + H3 simultaneously)
if(ASYNC_NET_HAS_SSL AND ASYNC_NET_HAS_HTTP2 AND ASYNC_NET_HAS_HTTP3)
    add_executable(http_multi_server http_multi_server.cpp)
    target_link_libraries(http_multi_server PRIVATE async_net)
endif()

# Multi-protocol HTTP client (H1 + H2 + H3 via URL scheme)
if(ASYNC_NET_HAS_SSL AND ASYNC_NET_HAS_HTTP2)
    add_executable(http_multi_client http_multi_client.cpp)
    target_link_libraries(http_multi_client PRIVATE async_net)
endif()

# IM server + client simulator (long-polling push & offline messages)
add_executable(im_server im_server.cpp)
target_link_libraries(im_server PRIVATE async_net)

add_executable(im_client_sim im_client_sim.cpp)
target_link_libraries(im_client_sim PRIVATE async_net)
