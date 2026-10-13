find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

protobuf_generate_cpp(CABINFLOW_PROTOCOL_PROTO_SOURCES
    CABINFLOW_PROTOCOL_PROTO_HEADERS
    proto/runtime_envelope.proto
    proto/control.proto
    proto/delivery.proto
    proto/runtime_gateway.proto
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

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/runtime_gateway.grpc.pb.cc
           ${CMAKE_CURRENT_BINARY_DIR}/runtime_gateway.grpc.pb.h
    COMMAND protobuf::protoc
    ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
         ${CMAKE_CURRENT_SOURCE_DIR}/proto/runtime_gateway.proto
    DEPENDS proto/runtime_gateway.proto proto/runtime_envelope.proto
    VERBATIM)
add_library(cabinflow_grpc_protocol ${CMAKE_CURRENT_BINARY_DIR}/runtime_gateway.grpc.pb.cc)
add_library(CabinFlow::GrpcProtocol ALIAS cabinflow_grpc_protocol)
target_include_directories(cabinflow_grpc_protocol PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cabinflow_grpc_protocol PUBLIC CabinFlow::Protocol gRPC::grpc++)
