add_executable(runtime_build_sanity unit/core/runtime_build_sanity.cpp)
target_link_libraries(runtime_build_sanity PRIVATE CabinFlow::Runtime)

add_test(NAME runtime_build_sanity COMMAND runtime_build_sanity)

add_test(NAME legacy_dependency_inventory_test
    COMMAND ${CMAKE_COMMAND}
        "-DRUNTIME_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/contract/legacy_dependency_inventory_test.cmake"
)

add_executable(event_loop_test unit/net/event_loop_test.cpp)
target_link_libraries(event_loop_test PRIVATE CabinFlow::Net)

add_test(NAME event_loop_test COMMAND event_loop_test)

add_executable(buffer_test unit/net/buffer_test.cpp)
target_link_libraries(buffer_test PRIVATE CabinFlow::Net)
target_include_directories(buffer_test PRIVATE ${PROJECT_SOURCE_DIR}/net/src)

add_test(NAME buffer_test COMMAND buffer_test)

add_executable(inet_address_test unit/net/inet_address_test.cpp)
target_link_libraries(inet_address_test PRIVATE CabinFlow::Net)
target_include_directories(inet_address_test PRIVATE ${PROJECT_SOURCE_DIR}/net/src)

add_test(NAME inet_address_test COMMAND inet_address_test)

add_executable(socket_test unit/net/socket_test.cpp)
target_link_libraries(socket_test PRIVATE CabinFlow::Net)
target_include_directories(socket_test PRIVATE ${PROJECT_SOURCE_DIR}/net/src)

add_test(NAME socket_test COMMAND socket_test)

add_executable(tcp_connection_test unit/net/tcp_connection_test.cpp)
target_link_libraries(tcp_connection_test PRIVATE CabinFlow::Net)
target_include_directories(tcp_connection_test PRIVATE ${PROJECT_SOURCE_DIR}/net/src)

add_test(NAME tcp_connection_test COMMAND tcp_connection_test)

add_executable(tcp_server_test unit/net/tcp_server_test.cpp)
target_link_libraries(tcp_server_test PRIVATE CabinFlow::Net)

add_test(NAME tcp_server_test COMMAND tcp_server_test)

add_executable(tcp_client_test unit/net/tcp_client_test.cpp)
target_link_libraries(tcp_client_test PRIVATE CabinFlow::Net)

add_test(NAME tcp_client_test COMMAND tcp_client_test)

add_executable(event_loop_thread_pool_test unit/net/event_loop_thread_pool_test.cpp)
target_link_libraries(event_loop_thread_pool_test PRIVATE CabinFlow::Net)
target_include_directories(event_loop_thread_pool_test PRIVATE ${PROJECT_SOURCE_DIR}/net/src)

add_test(NAME event_loop_thread_pool_test COMMAND event_loop_thread_pool_test)

add_executable(metrics_test unit/observability/metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE CabinFlow::Observability)

add_test(NAME metrics_test COMMAND metrics_test)

add_executable(message_codec_test unit/protocol/message_codec_test.cpp)
target_link_libraries(message_codec_test PRIVATE CabinFlow::Protocol)

add_test(NAME message_codec_test COMMAND message_codec_test)

add_executable(gateway_framer_contract_test contract/gateway_framer_contract_test.cpp)
target_link_libraries(gateway_framer_contract_test PRIVATE CabinFlow::Gateway)

add_test(NAME gateway_framer_contract_test COMMAND gateway_framer_contract_test)

add_executable(control_plane_contract_test contract/control_plane_contract_test.cpp)
target_link_libraries(control_plane_contract_test PRIVATE CabinFlow::Gateway)
target_include_directories(control_plane_contract_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME control_plane_contract_test COMMAND control_plane_contract_test)

add_executable(delivery_error_contract_test contract/delivery_error_contract_test.cpp)
target_link_libraries(delivery_error_contract_test PRIVATE CabinFlow::Gateway)
target_include_directories(delivery_error_contract_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME delivery_error_contract_test COMMAND delivery_error_contract_test)

add_executable(target_runtime_contract_test contract/target_runtime_contract_test.cpp)
target_link_libraries(target_runtime_contract_test
    PRIVATE
        CabinFlow::Runtime
        CabinFlow::TransportInMemory
)
target_include_directories(target_runtime_contract_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME target_runtime_contract_test COMMAND target_runtime_contract_test)

add_executable(control_gateway_tcp_test integration/control_gateway_tcp_test.cpp)
target_link_libraries(control_gateway_tcp_test
    PRIVATE
        CabinFlow::Gateway
        CabinFlow::TransportInMemory
)

add_test(NAME control_gateway_tcp_test COMMAND control_gateway_tcp_test)

add_executable(data_plane_gateway_tcp_test integration/data_plane_gateway_tcp_test.cpp)
target_link_libraries(data_plane_gateway_tcp_test
    PRIVATE
        CabinFlow::Runtime
        CabinFlow::Gateway
        CabinFlow::AgentProtocol
        CabinFlow::DialogueTextNode
        CabinFlow::TransportInMemory
)
target_include_directories(data_plane_gateway_tcp_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME data_plane_gateway_tcp_test COMMAND data_plane_gateway_tcp_test)

add_executable(session_ledger_test unit/core/session_ledger_test.cpp)
target_link_libraries(session_ledger_test PRIVATE CabinFlow::Runtime)
target_include_directories(session_ledger_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME session_ledger_test COMMAND session_ledger_test)

add_executable(bounded_queue_test unit/core/bounded_queue_test.cpp)
target_link_libraries(bounded_queue_test PRIVATE CabinFlow::Runtime)

add_test(NAME bounded_queue_test COMMAND bounded_queue_test)

add_executable(cancellation_test unit/core/cancellation_test.cpp)
target_link_libraries(cancellation_test PRIVATE CabinFlow::Runtime)

add_test(NAME cancellation_test COMMAND cancellation_test)

add_executable(runtime_test unit/core/runtime_test.cpp)
target_link_libraries(runtime_test
    PRIVATE
        CabinFlow::Runtime
        CabinFlow::TransportInMemory
)
target_include_directories(runtime_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME runtime_test COMMAND runtime_test)

add_executable(runtime_lifecycle_contract_test
    contract/runtime_lifecycle_contract_test.cpp
)
target_link_libraries(runtime_lifecycle_contract_test
    PRIVATE CabinFlow::Runtime CabinFlow::TransportInMemory
)
target_include_directories(runtime_lifecycle_contract_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support
)
add_test(NAME runtime_lifecycle_contract_test COMMAND runtime_lifecycle_contract_test)
set_tests_properties(runtime_lifecycle_contract_test PROPERTIES TIMEOUT 30)

add_executable(in_memory_transport_test unit/transport/in_memory_transport_test.cpp)
target_link_libraries(in_memory_transport_test
    PRIVATE
        CabinFlow::TransportInMemory
)
target_include_directories(in_memory_transport_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME in_memory_transport_test COMMAND in_memory_transport_test)

add_executable(two_node_flow_test integration/two_node_flow_test.cpp)
target_link_libraries(two_node_flow_test
    PRIVATE
        CabinFlow::Runtime
        CabinFlow::TransportInMemory
)
target_include_directories(two_node_flow_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME two_node_flow_test COMMAND two_node_flow_test)

add_executable(cancellation_flow_test integration/cancellation_flow_test.cpp)
target_link_libraries(cancellation_flow_test
    PRIVATE
        CabinFlow::Runtime
        CabinFlow::TransportInMemory
)
target_include_directories(cancellation_flow_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_test(NAME cancellation_flow_test COMMAND cancellation_flow_test)

add_executable(zmq_transport_test integration/zmq_transport_test.cpp)
target_link_libraries(zmq_transport_test
    PRIVATE
        CabinFlow::TransportZmq
)

add_test(NAME zmq_transport_test COMMAND zmq_transport_test)

add_test(NAME runtime_demo_integration
    COMMAND ${CMAKE_COMMAND}
        "-DRUNTIME_DEMO=$<TARGET_FILE:runtime_demo>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/runtime_demo_output.cmake"
)

add_test(NAME fake_voice_demo_integration
    COMMAND ${CMAKE_COMMAND}
        "-DFAKE_VOICE_DEMO=$<TARGET_FILE:fake_voice_demo>"
        "-DFAKE_VOICE_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/fake-voice-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/fake_voice_demo_output.cmake"
)

# 压力程序不加入默认 all/CTest；它用于手动复现满队列下的并发不变量。
add_executable(bounded_queue_stress_test EXCLUDE_FROM_ALL
    stress/bounded_queue_stress_test.cpp
)
target_link_libraries(bounded_queue_stress_test PRIVATE CabinFlow::Runtime)
target_compile_features(bounded_queue_stress_test PRIVATE cxx_std_17)
