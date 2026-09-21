cmake_minimum_required(VERSION 3.22)

project(CabinFlowRuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# The existing components retain their standalone build paths during the
# runtime migration. They are intentionally not included from this root build
# until their dependencies and interfaces are brought under test coverage.
add_subdirectory(protocol)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../agent/protocol
                 ${CMAKE_CURRENT_BINARY_DIR}/agent-protocol)
add_subdirectory(observability)
add_subdirectory(transport)
add_subdirectory(net)
add_subdirectory(core)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../agent/dialogue
                 ${CMAKE_CURRENT_BINARY_DIR}/agent-dialogue)
add_subdirectory(gateway)
add_subdirectory(apps/runtime_demo)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
