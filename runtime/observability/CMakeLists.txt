add_library(cabinflow_observability
    src/logger.cpp
    src/metrics.cpp
)
add_library(CabinFlow::Observability ALIAS cabinflow_observability)

target_include_directories(cabinflow_observability
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(cabinflow_observability PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cabinflow_observability PRIVATE -Wall -Wextra -Wpedantic)
endif()
