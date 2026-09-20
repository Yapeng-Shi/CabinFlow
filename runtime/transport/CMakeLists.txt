add_library(cabinflow_transport_api INTERFACE)
add_library(CabinFlow::TransportApi ALIAS cabinflow_transport_api)

target_include_directories(cabinflow_transport_api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_transport_api
    INTERFACE
        CabinFlow::Protocol
)

target_compile_features(cabinflow_transport_api INTERFACE cxx_std_17)

add_subdirectory(in_memory)
add_subdirectory(zmq)
