#pragma once

#include "cabinflow/agent/inference/backends.hpp"

#include <memory>
#include <string>

namespace cabinflow::agent::inference {

class SherpaAsrBackend final : public AsrBackend {
public:
    explicit SherpaAsrBackend(std::string model_directory);
    ~SherpaAsrBackend() override;
    BackendResult<std::string> transcribe(
        std::string_view wav, const CancellationCheck& cancelled) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LlamaBackend final : public LlmBackend {
public:
    LlamaBackend(std::string model_path, int threads, int context_size, int max_new_tokens);
    ~LlamaBackend() override;
    BackendResult<std::string> generate(
        std::string_view text, const CancellationCheck& cancelled) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 应用装配线程创建/销毁；每个进程只允许一次 Python 解释器生命周期。
class PythonRagBackend final : public RagBackend {
public:
    PythonRagBackend(std::string site_packages, std::string model_directory,
                     std::string index_directory, int threads);
    ~PythonRagBackend() override;
    BackendResult<RagSearchResult> search(
        std::string_view query, const CancellationCheck& cancelled) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SherpaTtsBackend final : public TtsBackend {
public:
    explicit SherpaTtsBackend(std::string model_directory);
    ~SherpaTtsBackend() override;
    BackendResult<WavAudio> synthesize(
        std::string_view text, const CancellationCheck& cancelled) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::agent::inference
