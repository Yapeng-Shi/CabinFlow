add_library(cabinflow_runtime
    src/cancellation.cpp
    src/clock.cpp
    src/node.cpp
    src/runtime.cpp
    src/session_ledger.cpp
    src/unit_registry.cpp
)
add_library(CabinFlow::Runtime ALIAS cabinflow_runtime)

target_include_directories(cabinflow_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_runtime
    PUBLIC
        CabinFlow::Observability
        CabinFlow::Protocol
        CabinFlow::TransportApi
)

target_compile_features(cabinflow_runtime PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_runtime)
