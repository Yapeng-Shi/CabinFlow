find_package(Protobuf REQUIRED)

protobuf_generate_cpp(CABINFLOW_PROTOCOL_PROTO_SOURCES
    CABINFLOW_PROTOCOL_PROTO_HEADERS
    proto/runtime_envelope.proto
    proto/control.proto
    proto/delivery.proto
)

add_library(cabinflow_protocol
    src/message_codec.cpp
    ${CABINFLOW_PROTOCOL_PROTO_SOURCES}
    ${CABINFLOW_PROTOCOL_PROTO_HEADERS}
)
add_library(CabinFlow::Protocol ALIAS cabinflow_protocol)

target_include_directories(cabinflow_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(cabinflow_protocol PUBLIC protobuf::libprotobuf)

target_compile_features(cabinflow_protocol PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_protocol)
