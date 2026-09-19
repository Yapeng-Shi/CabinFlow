cmake_minimum_required(VERSION 3.22)

project(CabinFlowRuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# The existing components retain their standalone build paths during the
# runtime migration. They are intentionally not included from this root build
# until their dependencies and interfaces are brought under test coverage.
add_subdirectory(runtime)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
