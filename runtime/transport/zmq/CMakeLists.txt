find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBZMQ REQUIRED IMPORTED_TARGET libzmq)

add_library(cabinflow_transport_zmq
    src/zmq_transport.cpp
)
add_library(CabinFlow::TransportZmq ALIAS cabinflow_transport_zmq)

target_include_directories(cabinflow_transport_zmq
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_transport_zmq
    PUBLIC
        CabinFlow::TransportApi
    PRIVATE
        PkgConfig::LIBZMQ
        Threads::Threads
)

target_compile_features(cabinflow_transport_zmq PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cabinflow_transport_zmq PRIVATE -Wall -Wextra -Wpedantic)
endif()
