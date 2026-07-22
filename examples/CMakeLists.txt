
# UDP broadcast
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

# HTTP examples (unified multi-protocol: H1 + H2 + H3)
add_executable(http_server http_server.cpp)
target_link_libraries(http_server PRIVATE async_net)

add_executable(http_client http_client.cpp)
target_link_libraries(http_client PRIVATE async_net)

# WebSocket echo server
add_executable(websocket_echo websocket_echo.cpp)
target_link_libraries(websocket_echo PRIVATE async_net)

# IM server + client simulator (long-polling push & offline messages)
add_executable(im_server im_server.cpp)
target_link_libraries(im_server PRIVATE async_net)

add_executable(im_client_sim im_client_sim.cpp)
target_link_libraries(im_client_sim PRIVATE async_net)

# gRPC examples (with Protocol Buffers)
if(ASYNC_NET_HAS_GRPC)
    # Generate C++ code from echo.proto via protoc
    async_net_protobuf_library(
        NAME grpc_echo_proto
        PROTOS grpc/echo.proto
    )

    add_executable(grpc_echo_server grpc_echo_server.cpp)
    target_link_libraries(grpc_echo_server PRIVATE async_net grpc_echo_proto)
    target_include_directories(grpc_echo_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated_grpc_echo_proto)

    add_executable(grpc_echo_client grpc_echo_client.cpp)
    target_link_libraries(grpc_echo_client PRIVATE async_net grpc_echo_proto)
    target_include_directories(grpc_echo_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated_grpc_echo_proto)
endif()

# FRPC examples (with FlatBuffers)
if(ASYNC_NET_HAS_FRPC)
    # Generate C++ code from echo.fbs via flatc
    async_net_flatbuffers_library(
        NAME frpc_echo_fbs
        FBS_FILES frpc/echo.fbs
    )

    add_executable(frpc_echo_server frpc_echo_server.cpp)
    target_link_libraries(frpc_echo_server PRIVATE async_net frpc_echo_fbs)
    target_include_directories(frpc_echo_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated_frpc_echo_fbs)

    add_executable(frpc_echo_client frpc_echo_client.cpp)
    target_link_libraries(frpc_echo_client PRIVATE async_net frpc_echo_fbs)
    target_include_directories(frpc_echo_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated_frpc_echo_fbs)
endif()
