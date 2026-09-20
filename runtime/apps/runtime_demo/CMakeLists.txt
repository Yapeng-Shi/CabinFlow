add_library(runtime_demo_nodes
    text_source_node.cpp
    echo_processor_node.cpp
)

target_include_directories(runtime_demo_nodes
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(runtime_demo_nodes
    PUBLIC
        CabinFlow::Observability
        CabinFlow::Runtime
        CabinFlow::TransportApi
)

target_compile_features(runtime_demo_nodes PUBLIC cxx_std_17)

add_executable(runtime_demo main.cpp)
target_link_libraries(runtime_demo
    PRIVATE
        runtime_demo_nodes
        CabinFlow::TransportInMemory
)
