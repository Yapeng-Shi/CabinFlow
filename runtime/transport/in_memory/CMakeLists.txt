add_library(cabinflow_transport_in_memory
    src/in_memory_transport.cpp
)
add_library(CabinFlow::TransportInMemory ALIAS cabinflow_transport_in_memory)

target_include_directories(cabinflow_transport_in_memory
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_transport_in_memory
    PUBLIC
        CabinFlow::TransportApi
)

target_compile_features(cabinflow_transport_in_memory PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_transport_in_memory)
