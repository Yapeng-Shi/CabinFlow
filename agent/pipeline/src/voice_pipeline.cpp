#include <cabinflow/agent/voice_pipeline.hpp>

#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <cockpit_audio.pb.h>
#include <cockpit_text.pb.h>
#include <control.pb.h>

#include <cabinflow/agent/dialogue_text_node.hpp>
#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/protocol/message_codec.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

namespace cabinflow::agent {
namespace {
using protocol::Message;
using runtime::MessageHandlingResult;
using inference::BackendError;
using StageResult = inference::BackendResult<std::string>;
constexpr std::size_t kMaxTextBytes = 16 * 1024;
constexpr std::size_t kMaxFrameBytes = 4 * 1024 * 1024;
constexpr std::uint32_t kTaskTtlMs = 300'000;

std::string new_trace_id() {
    std::random_device random;
    std::ostringstream result;
    result << "voice-" << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) result << std::setw(8) << random();
    return result.str();
}

StageResult cancelled_result() {
    return {{}, BackendError::kCancelled, "work_cancelled"};
}

template <class Proto>
StageResult serialize(const Proto& value) {
    std::string bytes;
    if (!value.SerializeToString(&bytes))
        return {{}, BackendError::kInferenceFailed, "business_protobuf_encode_failed"};
    return {std::move(bytes), BackendError::kNone, {}};
}
}  // namespace

struct VoicePipeline::State {
    enum class Stage { kAsr, kLlm, kTts };

    class ModelNode final : public runtime::TargetNode {
    public:
        ModelNode(State& owner, Stage stage) : owner_(owner), stage_(stage) {}
        std::string_view name() const noexcept override {
            switch (stage_) {
                case Stage::kAsr: return "asr.primary";
                case Stage::kLlm: return "llm.primary";
                case Stage::kTts: return "tts.primary";
            }
            return {};
        }
        runtime::RuntimeError start(runtime::NodeContext& context) override {
            context_ = &context;
            return runtime::RuntimeError::kNone;
        }
        void stop() noexcept override { context_ = nullptr; }
        MessageHandlingResult on_message(const Message& message) noexcept override {
            try {
                if (!context_) return MessageHandlingResult::kInvalidPayload;
                const auto token = context_->cancellation_token(
                    message.envelope.session_id, message.envelope.work_id);
                const inference::CancellationCheck cancelled = [this, token] {
                    return token.cancelled() || owner_.is_cancelled();
                };
                if (cancelled()) {
                    owner_.publish(cancelled_result());
                    return MessageHandlingResult::kHandled;
                }
                if (stage_ == Stage::kAsr) {
                    if (message.envelope.topic != "cockpit.audio.input")
                        return MessageHandlingResult::kUnsupportedTopic;
                    v1::AudioInput input;
                    if (!input.ParseFromString(message.payload) || input.wav_bytes().empty())
                        return MessageHandlingResult::kInvalidPayload;
                    auto result = owner_.asr->transcribe(input.wav_bytes(), cancelled);
                    if (!result) owner_.publish({{}, result.error, std::move(result.detail)});
                    else if (result.value.empty()) owner_.publish({{}, BackendError::kInferenceFailed, "empty_asr_transcript"});
                    else {
                        v1::TextInput output;
                        output.set_text(std::move(result.value));
                        owner_.publish(serialize(output));
                    }
                } else if (stage_ == Stage::kLlm) {
                    if (message.envelope.topic != "cockpit.llm.input")
                        return MessageHandlingResult::kUnsupportedTopic;
                    v1::TextInput input;
                    if (!input.ParseFromString(message.payload) || input.text().empty() || input.text().size() > kMaxTextBytes)
                        return MessageHandlingResult::kInvalidPayload;
                    auto result = owner_.llm->generate(input.text(), cancelled);
                    if (!result) owner_.publish({{}, result.error, std::move(result.detail)});
                    else if (result.value.empty()) owner_.publish({{}, BackendError::kInferenceFailed, "empty_llm_answer"});
                    else {
                        v1::SpeechText output;
                        output.set_text(std::move(result.value));
                        output.set_request_message_id(message.envelope.message_id);
                        owner_.publish(serialize(output));
                    }
                } else {
                    if (message.envelope.topic != "cockpit.speech.text")
                        return MessageHandlingResult::kUnsupportedTopic;
                    v1::SpeechText input;
                    if (!input.ParseFromString(message.payload) || input.text().empty() || input.request_message_id().empty())
                        return MessageHandlingResult::kInvalidPayload;
                    auto result = owner_.tts->synthesize(input.text(), cancelled);
                    if (!result) owner_.publish({{}, result.error, std::move(result.detail)});
                    else if (result.value.bytes.empty()) owner_.publish({{}, BackendError::kInferenceFailed, "empty_tts_audio"});
                    else {
                        v1::AudioOutput output;
                        output.set_request_message_id(input.request_message_id());
                        output.set_wav_bytes(std::move(result.value.bytes));
                        owner_.publish(serialize(output));
                    }
                }
                return MessageHandlingResult::kHandled;
            } catch (const std::exception& error) {
                // noexcept 节点边界终止当前阶段，不把 SDK/资源错误伪装成坏 payload 或继续链路。
                owner_.publish({{}, BackendError::kInferenceFailed, error.what()});
                return MessageHandlingResult::kHandled;
            }
        }
    private:
        State& owner_;
        Stage stage_;
        runtime::NodeContext* context_{nullptr};
    };

    std::unique_ptr<inference::AsrBackend> asr;
    std::unique_ptr<inference::LlmBackend> llm;
    std::unique_ptr<inference::TtsBackend> tts;
    std::mutex run_mutex;
    std::mutex mutex;
    std::condition_variable changed;
    bool active{false};
    bool cancellation_requested{false};
    bool handler_finished{false};
    std::optional<StageResult> stage_result;
    runtime::SteadyClock clock;
    transport::InMemoryTransport transport;
    observability::ConsoleLogger logger;
    runtime::UnitRegistry registry;
    gateway::ControlService control{registry};
    gateway::ControlEnvelopeValidator validator{clock};
    runtime::Runtime runtime{transport, clock, logger};

    State(std::unique_ptr<inference::AsrBackend> a,
          std::unique_ptr<inference::LlmBackend> l,
          std::unique_ptr<inference::TtsBackend> t)
        : asr(std::move(a)), llm(std::move(l)), tts(std::move(t)) {
        if (!asr || !llm || !tts) throw std::invalid_argument("all three model backends are required");
        for (const auto* unit : {"asr.primary", "dialogue.primary", "llm.primary", "tts.primary"}) {
            if (!registry.register_unit({unit, {unit}, 1}))
                throw std::runtime_error("cannot register pipeline unit");
        }
        for (auto stage : {Stage::kAsr, Stage::kLlm, Stage::kTts}) {
            if (runtime.add_target_node(std::make_unique<ModelNode>(*this, stage), 2) != runtime::RuntimeError::kNone)
                throw std::runtime_error("cannot register model target");
        }
        if (runtime.add_target_node(std::make_unique<DialogueTextNode>(
                [this](const auto&, std::string_view, std::string payload) {
                    publish({std::move(payload), BackendError::kNone, {}});
                    return true;
                }), 2) != runtime::RuntimeError::kNone ||
            runtime.start() != runtime::RuntimeError::kNone)
            throw std::runtime_error("cannot start voice pipeline");
    }
    ~State() {
        // sink/模型先声明、Runtime 后声明；显式 join 后再销毁引用目标，不能在回调里 stop。
        runtime.stop();
    }
    bool is_cancelled() {
        std::lock_guard<std::mutex> lock(mutex);
        return cancellation_requested;
    }
    void publish(StageResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        // 发布与 cancel 使用同一锁决定胜者；同步 SDK 返回的晚到结果不成为有效输出。
        stage_result = cancellation_requested ? cancelled_result() : std::move(result);
    }
    StageResult deliver(Message message) {
        const auto encoded = protocol::encode_message(message);
        if (!encoded || encoded.bytes.size() > kMaxFrameBytes)
            return {{}, BackendError::kInvalidInput, "encoded_runtime_frame_exceeds_limit_or_invalid"};
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancellation_requested) return cancelled_result();
            handler_finished = false;
            stage_result.reset();
        }
        auto reserved = runtime.reserve_target(message.envelope.target_node);
        if (!reserved) return {{}, BackendError::kInferenceFailed, "target_reservation_failed"};
        const auto admitted = runtime.admit_reserved(std::move(reserved.reservation), std::move(message),
            [this](runtime::Runtime::TargetDeliveryFailure reason) {
                const bool cancelled = reason == runtime::Runtime::TargetDeliveryFailure::kSessionCancelled ||
                                       reason == runtime::Runtime::TargetDeliveryFailure::kWorkCancelled;
                publish({{}, cancelled ? BackendError::kCancelled : BackendError::kInferenceFailed,
                         "runtime_delivery_failure=" + std::to_string(static_cast<int>(reason))});
            }, [this] {
                std::lock_guard<std::mutex> lock(mutex);
                handler_finished = true;
                changed.notify_one();
            });
        if (!admitted.admitted)
            return {{}, BackendError::kInferenceFailed, "runtime_admission=" + std::string(runtime::to_string(admitted.delivery_result))};
        std::unique_lock<std::mutex> lock(mutex);
        // 只等 completion，不把 publish 当作 handler 已退出；同步库不支持硬中断时等其实际返回。
        changed.wait(lock, [this] { return handler_finished; });
        if (cancellation_requested) return cancelled_result();
        if (!stage_result) return {{}, BackendError::kInferenceFailed, "target_completed_without_result"};
        return std::move(*stage_result);
    }
    protocol::v1::ControlResponse command(Message envelope, const protocol::v1::ControlRequest& request) {
        envelope.envelope.target_node = "runtime.control";
        envelope.envelope.topic = "control.request";
        envelope.payload = request.SerializeAsString();
        auto checked = validator.validate(envelope);
        if (!checked || !checked.request) throw std::runtime_error("invalid pipeline control envelope");
        auto response = control.handle(*checked.request).response;
        if (response.has_error()) throw std::runtime_error(response.error().message());
        return response;
    }
};

VoicePipeline::VoicePipeline(std::unique_ptr<inference::AsrBackend> asr,
                             std::unique_ptr<inference::LlmBackend> llm,
                             std::unique_ptr<inference::TtsBackend> tts)
    : state_(std::make_unique<State>(std::move(asr), std::move(llm), std::move(tts))) {}
VoicePipeline::~VoicePipeline() = default;

bool VoicePipeline::cancel() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->active) return false;
    state_->cancellation_requested = true;
    return true;
}

VoiceResult VoicePipeline::run_text(std::string text) { return run(false, std::move(text)); }
VoiceResult VoicePipeline::run_wav(inference::WavAudio audio) { return run(true, std::move(audio.bytes)); }

VoiceResult VoicePipeline::run(bool audio_input, std::string input) {
    std::unique_lock<std::mutex> one_run(state_->run_mutex, std::try_to_lock);
    if (!one_run.owns_lock()) { VoiceResult busy; busy.status = VoiceStatus::kBusy; busy.detail = "task_already_active"; return busy; }
    VoiceResult result;
    if (input.empty() || (!audio_input && input.size() > kMaxTextBytes)) {
        result.detail = "empty_or_oversized_input";
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active = true;
        state_->cancellation_requested = false;
    }
    std::uint64_t sequence = 0;
    auto make = [&](std::string_view topic, std::string_view target, std::string payload) {
        Message message;
        message.envelope.trace_id = result.trace_id;
        message.envelope.session_id = result.session_id;
        message.envelope.work_id = result.work_id;
        message.envelope.message_id = result.trace_id + ":" + std::to_string(sequence++);
        message.envelope.source_node = "voice.pipeline";
        message.envelope.target_node = std::string(target);
        message.envelope.topic = std::string(topic);
        message.envelope.sequence = 0;
        message.envelope.created_monotonic_ns = state_->clock.now_monotonic_ns();
        message.envelope.ttl_ms = kTaskTtlMs;
        message.envelope.is_final = true;
        message.payload = std::move(payload);
        return message;
    };
    try {
        result.trace_id = new_trace_id();
        result.session_id = result.trace_id + ":session";
        const std::string unit = audio_input ? "asr.primary" : "dialogue.primary";
        protocol::v1::ControlRequest control;
        // Registry 的稳定 Unit 注册在实例内保留；每次任务仍由 ControlService 创建全新 work。
        control.mutable_setup()->set_unit_id(unit);
        result.work_id = state_->command(make("control.request", "runtime.control", {}), control).setup().work().work_id();
        StageResult stage;
        v1::TextInput text;
        if (audio_input) {
            v1::AudioInput audio;
            audio.set_wav_bytes(std::move(input));
            auto first = make("cockpit.audio.input", "asr.primary", audio.SerializeAsString());
            result.request_message_id = first.envelope.message_id;
            stage = state_->deliver(std::move(first));
            if (!stage) throw std::runtime_error(stage.detail);
            if (!text.ParseFromString(stage.value) || text.text().empty()) throw std::runtime_error("invalid_asr_output");
        } else text.set_text(std::move(input));
        result.transcript = text.text();
        auto router_input = make("cockpit.text.input", "dialogue.primary", text.SerializeAsString());
        if (!audio_input) result.request_message_id = router_input.envelope.message_id;
        stage = state_->deliver(std::move(router_input));
        if (!stage) throw std::runtime_error(stage.detail);
        v1::TextOutput route;
        if (!route.ParseFromString(stage.value)) throw std::runtime_error("invalid_router_output");
        if (route.intent() == v1::COCKPIT_INTENT_UNRECOGNIZED) {
            // LLM 使用保留的原问题；不同 topic 避免重复使用已 final 的 Router 输入流。
            stage = state_->deliver(make("cockpit.llm.input", "llm.primary", text.SerializeAsString()));
            if (!stage) throw std::runtime_error(stage.detail);
            v1::SpeechText answer;
            if (!answer.ParseFromString(stage.value) || answer.text().empty()) throw std::runtime_error("invalid_llm_output");
            result.answer = answer.text();
        } else result.answer = route.text();  // 当前是明确的“未执行车控”回执，不冒充 Vehicle 已执行。
        v1::SpeechText speech;
        speech.set_text(result.answer);
        speech.set_request_message_id(result.request_message_id);
        stage = state_->deliver(make("cockpit.speech.text", "tts.primary", speech.SerializeAsString()));
        if (!stage) throw std::runtime_error(stage.detail);
        v1::AudioOutput audio;
        if (!audio.ParseFromString(stage.value) || audio.request_message_id() != result.request_message_id || audio.wav_bytes().empty())
            throw std::runtime_error("invalid_tts_output_identity");
        // 验证的是整个最终帧体，而不是只计算 WAV 大小；后续 TCP 入口沿用同一上限。
        const auto final_frame = protocol::encode_message(make("cockpit.audio.output", "voice.client", stage.value));
        if (!final_frame || final_frame.bytes.size() > kMaxFrameBytes) throw std::runtime_error("output_frame_exceeds_limit");
        result.audio.bytes = audio.wav_bytes();
        result.status = VoiceStatus::kCompleted;
    } catch (const std::exception& error) {
        result.detail = error.what();
        result.status = VoiceStatus::kFailed;
    }
    if (!result.work_id.empty()) {
        protocol::v1::ControlRequest exit;
        exit.mutable_exit()->set_reason("voice pipeline task finalized");
        try { static_cast<void>(state_->command(make("control.request", "runtime.control", {}), exit)); }
        catch (const std::exception& error) { result.status = VoiceStatus::kFailed; result.detail = error.what(); }
        state_->runtime.cancel_work(result.session_id, result.work_id);
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancellation_requested) { result.status = VoiceStatus::kCancelled; result.detail = "work_cancelled"; }
        if (result.status != VoiceStatus::kCompleted) { result.audio.bytes.clear(); result.answer.clear(); }
        state_->active = false;
    }
    return result;
}

}  // namespace cabinflow::agent
