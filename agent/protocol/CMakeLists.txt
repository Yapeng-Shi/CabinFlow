find_package(Protobuf REQUIRED)

protobuf_generate_cpp(CABINFLOW_AGENT_PROTOCOL_PROTO_SOURCES
    CABINFLOW_AGENT_PROTOCOL_PROTO_HEADERS
    proto/cockpit_text.proto
)

add_library(cabinflow_agent_protocol
    ${CABINFLOW_AGENT_PROTOCOL_PROTO_SOURCES}
    ${CABINFLOW_AGENT_PROTOCOL_PROTO_HEADERS}
)
add_library(CabinFlow::AgentProtocol ALIAS cabinflow_agent_protocol)

target_include_directories(cabinflow_agent_protocol
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(cabinflow_agent_protocol PUBLIC protobuf::libprotobuf)
target_compile_features(cabinflow_agent_protocol PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_agent_protocol)
