set(CABINFLOW_INFERENCE_DIR "" CACHE PATH "Absolute directory of the prepared, locked inference SDKs")
if(NOT IS_ABSOLUTE "${CABINFLOW_INFERENCE_DIR}")
    message(FATAL_ERROR "CABINFLOW_INFERENCE_DIR must name the absolute prepared inference directory")
endif()

set(_sherpa "${CABINFLOW_INFERENCE_DIR}/sherpa-onnx-v1.11.3-linux-x64-shared")
set(_llama "${CABINFLOW_INFERENCE_DIR}/llama.cpp-a7a98e0fffed794396b3fbad4dcdbbc184963645")
set(_llama_build "${CABINFLOW_INFERENCE_DIR}/llama-build")

foreach(_file IN ITEMS
    "${_sherpa}/include/sherpa-onnx/c-api/c-api.h"
    "${_sherpa}/lib/libsherpa-onnx-c-api.so"
    "${_sherpa}/lib/libonnxruntime.so"
    "${_llama}/include/llama.h"
    "${_llama}/common/chat.h"
    "${_llama_build}/common/libcommon.a"
    "${_llama_build}/src/libllama.a"
    "${_llama_build}/ggml/src/libggml.a"
    "${_llama_build}/ggml/src/libggml-cpu.a"
    "${_llama_build}/ggml/src/libggml-base.a")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "Missing locked inference SDK artifact: ${_file}")
    endif()
endforeach()

add_library(cabinflow_native_backends STATIC src/native_backends.cpp src/wav.cpp)
add_library(CabinFlow::NativeBackends ALIAS cabinflow_native_backends)
target_compile_features(cabinflow_native_backends PUBLIC cxx_std_17)
target_include_directories(cabinflow_native_backends
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${_sherpa}/include" "${_llama}/include" "${_llama}/common"
            "${_llama}/ggml/include" "${_llama}/vendor")
find_package(Threads REQUIRED)
target_link_libraries(cabinflow_native_backends PRIVATE
    "${_llama_build}/common/libcommon.a"
    "${_llama_build}/src/libllama.a"
    "${_llama_build}/ggml/src/libggml.a"
    "${_llama_build}/ggml/src/libggml-cpu.a"
    "${_llama_build}/ggml/src/libggml-base.a"
    "${_sherpa}/lib/libsherpa-onnx-c-api.so"
    "${_sherpa}/lib/libonnxruntime.so"
    Threads::Threads ${CMAKE_DL_LIBS} m)
