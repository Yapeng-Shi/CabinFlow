#pragma once

#include <memory>
#include <optional>
#include <string>

#include <cabinflow/agent/vehicle_backend.hpp>
#include <cockpit_text.pb.h>
#include <cabinflow/agent/inference/backends.hpp>
#include <cabinflow/gateway/control_gateway.hpp>

namespace cabinflow::agent {

enum class VoiceStatus { kCompleted, kFailed, kCancelled, kBusy };

struct VehicleReceipt {
    bool simulated;
    std::optional<bool> climate_on;
    std::optional<bool> action_applied;
    std::optional<bool> left_front_window_open;
};

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
    std::optional<VehicleReceipt> vehicle;
    std::optional<v1::MusicCommand> music_command;
    bool vehicle_status{false};
};

// 编排拥有模型，Runtime 拥有独立 Target 队列/worker；UI/CLI 不包含 SDK 类型。
// run_* 在外部控制线程同步调用；cancel 可并发请求，调用方须等待 run 返回再销毁对象。
class VoicePipeline final {
public:
    VoicePipeline(std::unique_ptr<inference::AsrBackend> asr,
                  std::unique_ptr<inference::RagBackend> rag,
                  std::unique_ptr<inference::LlmBackend> llm,
                  std::unique_ptr<inference::TtsBackend> tts,
                  std::unique_ptr<VehicleBackend> vehicle);
    ~VoicePipeline();
    VoicePipeline(const VoicePipeline&) = delete;
    VoicePipeline& operator=(const VoicePipeline&) = delete;

    [[nodiscard]] VoiceResult run_text(std::string text);
    [[nodiscard]] VoiceResult run_wav(inference::WavAudio audio);
    [[nodiscard]] VoiceResult refresh_vehicle_state();
    // true 只表示取消请求被接受，不表示 SDK 调用已经退出。
    [[nodiscard]] bool cancel();

    [[nodiscard]] runtime::Runtime& runtime() noexcept;
    [[nodiscard]] runtime::UnitRegistry& registry() noexcept;
    [[nodiscard]] const runtime::Clock& clock() const noexcept;
    [[nodiscard]] gateway::DataTaskHooks hooks();
    // 只能由外部控制线程调用；停止准入、取消并等根 completion，最后才停止 worker。
    void shutdown();

private:
    struct State;
    std::unique_ptr<State> state_;
    VoiceResult run(bool audio_input, std::string input, bool status_query = false);
};

}  // namespace cabinflow::agent
