#pragma once

#include <memory>
#include <string>

#include <cabinflow/agent/inference/backends.hpp>

namespace cabinflow::agent {

enum class VoiceStatus { kCompleted, kFailed, kCancelled, kBusy };

struct VoiceResult {
    VoiceStatus status{VoiceStatus::kFailed};
    std::string detail;
    std::string trace_id;
    std::string session_id;
    std::string work_id;
    std::string request_message_id;
    std::string transcript;
    std::string answer;
    inference::WavAudio audio;
};

// 编排拥有模型，Runtime 拥有独立 Target 队列/worker；UI/CLI 不包含 SDK 类型。
// run_* 在外部控制线程同步调用；cancel 可并发请求，调用方须等待 run 返回再销毁对象。
class VoicePipeline final {
public:
    VoicePipeline(std::unique_ptr<inference::AsrBackend> asr,
                  std::unique_ptr<inference::LlmBackend> llm,
                  std::unique_ptr<inference::TtsBackend> tts);
    ~VoicePipeline();
    VoicePipeline(const VoicePipeline&) = delete;
    VoicePipeline& operator=(const VoicePipeline&) = delete;

    [[nodiscard]] VoiceResult run_text(std::string text);
    [[nodiscard]] VoiceResult run_wav(inference::WavAudio audio);
    // true 只表示取消请求被接受，不表示 SDK 调用已经退出。
    [[nodiscard]] bool cancel();

private:
    struct State;
    std::unique_ptr<State> state_;
    VoiceResult run(bool audio_input, std::string input);
};

}  // namespace cabinflow::agent
