#pragma once

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cabinflow::agent::inference {

enum class BackendError { kNone, kInvalidInput, kInferenceFailed, kCancelled };

template <class T>
struct BackendResult {
    T value;
    BackendError error{BackendError::kNone};
    std::string detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return error == BackendError::kNone;
    }
};

struct WavAudio { std::string bytes; };

struct RagDocument {
    std::uint32_t id{0};
    std::string section;
    std::string subsection;
    std::string text;
    float similarity{0};
};

struct RagSearchResult {
    // 无命中是成功完成检索但没有资料，与模型故障、取消和 LLM 回答分开。
    std::optional<RagDocument> match;
};
using CancellationCheck = std::function<bool()>;

// 端口只使用拥有数据的标准类型；SDK 对象、分词器和模型参数不跨过该边界。
// 调用同步完成，不保留输入 view 或取消回调；一个实例只由所属 Target worker 调用。
class AsrBackend {
public:
    virtual ~AsrBackend() = default;
    // 首版输入合同沿用 AudioInput：16 kHz、mono、PCM16 WAV；不自动转码。
    virtual BackendResult<std::string> transcribe(
        std::string_view wav, const CancellationCheck& cancelled) = 0;
};

class LlmBackend {
public:
    virtual ~LlmBackend() = default;
    virtual BackendResult<std::string> generate(
        std::string_view text, const CancellationCheck& cancelled) = 0;
};

class RagBackend {
public:
    virtual ~RagBackend() = default;
    virtual BackendResult<RagSearchResult> search(
        std::string_view query, const CancellationCheck& cancelled) = 0;
};

class TtsBackend {
public:
    virtual ~TtsBackend() = default;
    // 输出为完整 WAV，自带实际采样率；调用方不猜裸 PCM 格式。
    virtual BackendResult<WavAudio> synthesize(
        std::string_view text, const CancellationCheck& cancelled) = 0;
};

}  // namespace cabinflow::agent::inference
