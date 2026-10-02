if(NOT DEFINED RUNTIME_SOURCE_DIR)
    message(FATAL_ERROR "RUNTIME_SOURCE_DIR is required")
endif()

# 固定取证材料，而非运行会修改系统的旧安装脚本；指纹变化必须重新核对依赖记录。
file(SHA256 "${RUNTIME_SOURCE_DIR}/build.sh" script_hash)
if(NOT script_hash STREQUAL "d362096acac660c0b5fe6cd0c42a55b17bc6b7c8005fe7cbba3179d1bd8d2dd0")
    message(FATAL_ERROR "Legacy build.sh changed: refresh the dependency evidence")
endif()

foreach(input IN ITEMS
    docs/legacy-dependencies.md
    utils/json.hpp utils/sample_log.h
    node/test/CMakeLists.txt node/test/src/main.cpp
    sample/CMakeLists.txt sample/pub.cc sample/sub.cc
    sample/rpc_call.cc sample/rpc_server.cc
    sample/pz_rpc_call.cc sample/pz_rpc_server.cc sample/test.py sample/stress.py
    infra-controller/CMakeLists.txt unit-manager/CMakeLists.txt
    network/src/CMakeLists.txt hybrid-comm/src/pzmq_data.cpp)
    if(NOT EXISTS "${RUNTIME_SOURCE_DIR}/${input}")
        message(FATAL_ERROR "Required migration input is missing: ${input}")
    endif()
endforeach()

file(STRINGS "${RUNTIME_SOURCE_DIR}/CMakeLists.txt" root_lines)
foreach(line IN LISTS root_lines)
    # 只检查活动命令，注释里的迁移说明不应被误判为接入旧构建。
    if(line MATCHES "^[ \t]*add_subdirectory[ \t]*\\([^)]*(infra-controller|unit-manager|network|hybrid-comm|node/test|sample|utils)")
        message(FATAL_ERROR "Legacy input must not rejoin the root build: ${line}")
    endif()
endforeach()

message(STATUS "Legacy dependency inputs preserved; installation behavior remains unverified")
