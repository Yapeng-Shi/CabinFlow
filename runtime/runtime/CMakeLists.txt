add_library(cabinflow_runtime INTERFACE)
add_library(CabinFlow::Runtime ALIAS cabinflow_runtime)

target_include_directories(cabinflow_runtime
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(cabinflow_runtime INTERFACE cxx_std_17)
