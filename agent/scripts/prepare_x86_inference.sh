#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
inference_dir="${repo_dir}/runtime/build/inference"
sherpa_version="1.11.3"
llama_commit="a7a98e0fffed794396b3fbad4dcdbbc184963645"
mkdir -p "${inference_dir}/downloads"

fetch() {
    local filename="$1" url="$2"
    local target="${inference_dir}/downloads/${filename}"
    if [[ ! -f "${target}" ]]; then
        curl --fail --location --show-error --silent \
            --connect-timeout 15 --max-time 300 \
            --output "${target}.part" "${url}"
        mv -- "${target}.part" "${target}"
    fi
}

# 只接受固定发布包；缓存仍需验哈希，失败不会换镜像、版本或推理后端。
fetch "sherpa-onnx-v${sherpa_version}-linux-x64-shared.tar.bz2" \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${sherpa_version}/sherpa-onnx-v${sherpa_version}-linux-x64-shared.tar.bz2"
fetch "llama-a7a98e0.tar.gz" \
    "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/${llama_commit}"
(cd "${inference_dir}" && sha256sum --check "${repo_dir}/agent/models/x86-inference.sha256")
tar -xjf "${inference_dir}/downloads/sherpa-onnx-v${sherpa_version}-linux-x64-shared.tar.bz2" -C "${inference_dir}"
tar -xzf "${inference_dir}/downloads/llama-a7a98e0.tar.gz" -C "${inference_dir}"
(cd "${inference_dir}" && sha256sum --check "${repo_dir}/agent/models/sherpa-runtime.sha256")

# Sherpa 使用上游 CPU shared 发布包；llama.cpp 在本地编译，不接入 Runtime 根构建。
# 关闭模型网络下载和 GPU/宿主特化，烟测始终使用已校验的本地模型。
cmake -S "${inference_dir}/llama.cpp-${llama_commit}" \
    -B "${inference_dir}/llama-build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DLLAMA_BUILD_NUMBER=6500 -DLLAMA_BUILD_COMMIT="${llama_commit}" \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_CURL=OFF -DLLAMA_LLGUIDANCE=OFF \
    -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_CUDA=OFF \
    -DGGML_VULKAN=OFF -DGGML_BLAS=OFF -DGGML_RPC=OFF
cmake --build "${inference_dir}/llama-build" --target llama-cli -j2
# 本地构建的产物没有通用发布哈希；记录本次产物，烟测时检查其未被替换。
(cd "${inference_dir}" && sha256sum llama-build/bin/llama-cli > llama-cli.sha256)
printf 'inference dependencies ready under %s\n' "${inference_dir}"
