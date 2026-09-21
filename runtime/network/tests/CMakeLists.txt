cmake_minimum_required(VERSION 3.22)

project(LegacyNetworkCharacterization LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 旧源码包含 GNU VLA 扩展；characterization 构建保持其原有编译语义。
set(CMAKE_CXX_EXTENSIONS ON)

find_package(Threads REQUIRED)
find_package(glog CONFIG REQUIRED)

# Windows 挂载盘中的 Anaconda 可能通过 PATH 被 CMake 误发现；旧 Reactor
# 只能用与 WSL/GCC ABI 一致的原生 glog 构建，不能把跨平台依赖当作兼容回退。
if(glog_DIR MATCHES "^/mnt/")
    message(FATAL_ERROR
        "Legacy network characterization requires a native WSL glog, but found: ${glog_DIR}")
endif()

set(legacy_network_sources
    ../src/Acceptor.cc
    ../src/Buffer.cc
    ../src/Channel.cc
    ../src/Connector.cc
    ../src/EventLoop.cc
    ../src/EventLoopThread.cc
    ../src/EventLoopThreadPool.cc
    ../src/InetAddress.cc
    ../src/Poller.cc
    ../src/Socket.cc
    ../src/SocketsOps.cc
    ../src/TcpClient.cc
    ../src/TcpConnection.cc
    ../src/TcpServer.cc
    ../src/util.cc)

add_library(legacy_network STATIC ${legacy_network_sources})
target_include_directories(legacy_network PUBLIC ../include)
target_link_libraries(legacy_network PUBLIC Threads::Threads glog::glog)

add_executable(legacy_network_characterization_test
    legacy_network_characterization_test.cpp)
target_link_libraries(legacy_network_characterization_test PRIVATE legacy_network)

add_test(NAME legacy_network_characterization_test
         COMMAND legacy_network_characterization_test)
