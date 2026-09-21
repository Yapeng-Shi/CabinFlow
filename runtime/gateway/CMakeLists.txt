add_library(cabinflow_gateway
    src/control_envelope_validator.cpp
    src/control_gateway.cpp
    src/control_response_builder.cpp
    src/control_service.cpp
    src/data_envelope_validator.cpp
    src/delivery_error_builder.cpp
    src/message_id.cpp
    src/response_sequencer.cpp
    src/runtime_message_framer.cpp
)
add_library(CabinFlow::Gateway ALIAS cabinflow_gateway)

target_include_directories(cabinflow_gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_gateway
    PUBLIC
        CabinFlow::Protocol
        CabinFlow::Runtime
        CabinFlow::Net
)

target_compile_features(cabinflow_gateway PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_gateway)
