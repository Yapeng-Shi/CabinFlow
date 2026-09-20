add_executable(runtime_build_sanity unit/core/runtime_build_sanity.cpp)
target_link_libraries(runtime_build_sanity PRIVATE CabinFlow::Runtime)

add_test(NAME runtime_build_sanity COMMAND runtime_build_sanity)

add_executable(metrics_test unit/observability/metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE CabinFlow::Observability)

add_test(NAME metrics_test COMMAND metrics_test)

add_executable(message_codec_test unit/protocol/message_codec_test.cpp)
target_link_libraries(message_codec_test PRIVATE CabinFlow::Protocol)

add_test(NAME message_codec_test COMMAND message_codec_test)

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

add_executable(in_memory_transport_test integration/in_memory_transport_test.cpp)
target_link_libraries(in_memory_transport_test
    PRIVATE
        CabinFlow::TransportInMemory
)

add_test(NAME in_memory_transport_test COMMAND in_memory_transport_test)

add_executable(zmq_transport_test integration/zmq_transport_test.cpp)
target_link_libraries(zmq_transport_test
    PRIVATE
        CabinFlow::TransportZmq
)

add_test(NAME zmq_transport_test COMMAND zmq_transport_test)

add_test(NAME runtime_demo_integration COMMAND runtime_demo)
set_tests_properties(runtime_demo_integration PROPERTIES
    PASS_REGULAR_EXPRESSION
        "event=echo_completed.*trace_id=demo-trace.*session_id=driver-session.*work_id=demo-work.*message_id=demo-message.*detail=echo:hello_cabinflow"
)

# 压力程序不加入默认 all/CTest；它用于手动复现满队列下的并发不变量。
add_executable(bounded_queue_stress_test EXCLUDE_FROM_ALL
    stress/bounded_queue_stress_test.cpp
)
target_link_libraries(bounded_queue_stress_test PRIVATE CabinFlow::Runtime)
target_compile_features(bounded_queue_stress_test PRIVATE cxx_std_17)
