add_library(cabinflow_net
    src/acceptor.cpp
    src/buffer.cpp
    src/channel.cpp
    src/connector.cpp
    src/event_loop.cpp
    src/event_loop_thread.cpp
    src/event_loop_thread_pool.cpp
    src/inet_address.cpp
    src/poller.cpp
    src/socket.cpp
    src/tcp_connection.cpp
    src/tcp_client.cpp
    src/tcp_server.cpp
)
add_library(CabinFlow::Net ALIAS cabinflow_net)

target_include_directories(cabinflow_net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(cabinflow_net PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_net)
