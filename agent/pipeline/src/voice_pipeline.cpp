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
#include <cockpit_task.pb.h>
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
using DeliveryFailure = runtime::Runtime::TargetDeliveryFailure;
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

StageResult cancelled_result() { return {{}, BackendError::kCancelled, "work_cancelled"}; }

StageResult delivery_failure(DeliveryFailure reason) {
    if (reason == DeliveryFailure::kSessionCancelled || reason == DeliveryFailure::kWorkCancelled)
        return cancelled_result();
    return {{}, BackendError::kInferenceFailed,
            "runtime_delivery_failure=" + std::to_string(static_cast<int>(reason))};
}

template <class Proto>
StageResult serialize(const Proto& value) {
    std::string bytes;
    if (!value.SerializeToString(&bytes))
        return {{}, BackendError::kInferenceFailed, "business_protobuf_encode_failed"};
    return {std::move(bytes), BackendError::kNone, {}};
}

bool same_request(const protocol::MessageEnvelope& a, const protocol::MessageEnvelope& b) {
    return a.session_id == b.session_id && a.work_id == b.work_id && a.message_id == b.message_id;
}

VoiceResult result_identity(const protocol::MessageEnvelope& envelope) {
    VoiceResult result;
    result.trace_id = envelope.trace_id;
    result.session_id = envelope.session_id;
    result.work_id = envelope.work_id;
    result.request_message_id = envelope.message_id;
    return result;
}
}  // namespace

struct VoicePipeline::State {
    enum class Stage { kAsr, kDialogue, kLlm, kTts };

    class ModelNode final : public runtime::TargetNode {
    public:
        ModelNode(State& owner, Stage stage)
            : owner_(owner), stage_(stage), router_([&owner](const auto&, std::string_view, std::string payload) {
                  owner.publish({std::move(payload), BackendError::kNone, {}});
                  return true;
              }) {}
        std::string_view name() const noexcept override {
            switch (stage_) {
                case Stage::kAsr: return "asr.primary";
                case Stage::kDialogue: return "dialogue.primary";
                case Stage::kLlm: return "llm.primary";
                case Stage::kTts: return "tts.primary";
            }
            return {};
        }
        runtime::RuntimeError start(runtime::NodeContext& context) override {
            context_ = &context;
            return stage_ == Stage::kDialogue ? router_.start(context) : runtime::RuntimeError::kNone;
        }
        void stop() noexcept override {
            if (stage_ == Stage::kDialogue) router_.stop();
            context_ = nullptr;
        }
        MessageHandlingResult on_message(const Message& message) noexcept override {
            try {
                if (!context_) return MessageHandlingResult::kInvalidPayload;
                const auto token = context_->cancellation_token(message.envelope.session_id, message.envelope.work_id);
                const inference::CancellationCheck cancelled = [this, token] {
                    return token.cancelled() || owner_.is_cancelled();
                };
                if (owner_.is_root(message.envelope)) {
                    if (stage_ != Stage::kAsr && stage_ != Stage::kDialogue)
                        return MessageHandlingResult::kUnsupportedTopic;
                    return run_root(message, cancelled);
                }
                if (stage_ == Stage::kAsr) return MessageHandlingResult::kInvalidPayload;
                if (stage_ == Stage::kDialogue && message.envelope.topic != "cockpit.router.input")
                    return MessageHandlingResult::kUnsupportedTopic;
                if (cancelled()) { owner_.publish(cancelled_result()); return MessageHandlingResult::kHandled; }
                if (stage_ == Stage::kDialogue) return route(message);
                if (stage_ == Stage::kLlm) {
                    if (message.envelope.topic != "cockpit.llm.input") return MessageHandlingResult::kUnsupportedTopic;
                    v1::TextInput input;
                    if (!input.ParseFromString(message.payload) || input.text().empty() || input.text().size() > kMaxTextBytes)
                        return MessageHandlingResult::kInvalidPayload;
                    const auto started_ns = owner_.clock.now_monotonic_ns();
                    auto result = owner_.llm->generate(input.text(), cancelled);
                    const auto finished_ns = owner_.clock.now_monotonic_ns();
                    log_backend_call(message, result.error, finished_ns - started_ns);
                    if (!result) owner_.publish({{}, result.error, std::move(result.detail)});
                    else if (result.value.empty()) owner_.publish({{}, BackendError::kInferenceFailed, "empty_llm_answer"});
                    else {
                        v1::SpeechText output;
                        output.set_text(std::move(result.value));
                        output.set_request_message_id(message.envelope.message_id);
                        owner_.publish(serialize(output));
                    }
                } else {
                    if (message.envelope.topic != "cockpit.speech.text") return MessageHandlingResult::kUnsupportedTopic;
                    v1::SpeechText input;
                    if (!input.ParseFromString(message.payload) || input.text().empty() || input.request_message_id().empty())
                        return MessageHandlingResult::kInvalidPayload;
                    const auto started_ns = owner_.clock.now_monotonic_ns();
                    auto result = owner_.tts->synthesize(input.text(), cancelled);
                    const auto finished_ns = owner_.clock.now_monotonic_ns();
                    log_backend_call(message, result.error, finished_ns - started_ns);
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
                // noexcept 边界只产生明确失败，不把 SDK 异常转换成可重投的业务输入。
                if (owner_.is_root(message.envelope)) {
                    auto result = result_identity(message.envelope);
                    result.detail = error.what();
                    owner_.store_candidate(std::move(result));
                } else owner_.publish({{}, BackendError::kInferenceFailed, error.what()});
                return MessageHandlingResult::kHandled;
            }
        }

    private:
        void log_backend_call(const Message& message, BackendError error, std::uint64_t duration_ns) {
            std::string_view status;
            switch (error) {
                case BackendError::kNone: status = "kNone"; break;
                case BackendError::kInvalidInput: status = "kInvalidInput"; break;
                case BackendError::kInferenceFailed: status = "kInferenceFailed"; break;
                case BackendError::kCancelled: status = "kCancelled"; break;
            }
            std::string request_id;
            {
                // 根请求保持到所有 Backend 返回并完成清理；身份直接读取，不猜阶段 ID 编码。
                std::lock_guard<std::mutex> lock(owner_.mutex);
                if (!owner_.active_request ||
                    owner_.active_request->session_id != message.envelope.session_id ||
                    owner_.active_request->work_id != message.envelope.work_id)
                    throw std::logic_error("backend_measurement_without_active_request");
                request_id = owner_.active_request->message_id;
            }
            // 单调时间仅包住 Backend 调用；排队、模型加载、日志和输出序列化均不计入。
            owner_.logger.log({std::string(name()), "backend_call_completed", message.envelope.trace_id,
                message.envelope.session_id, message.envelope.work_id, message.envelope.message_id,
                "duration_ns=" + std::to_string(duration_ns) + " request_message_id=" + request_id,
                std::string(status)});
        }

        MessageHandlingResult route(const Message& message) {
            // 只有这个 wrapper 处理独立内部 topic；原意图节点仍是规则的唯一实现。
            Message input = message;
            input.envelope.topic = "cockpit.text.input";
            return router_.on_message(input);
        }

        MessageHandlingResult run_root(const Message& message, const inference::CancellationCheck& cancelled) {
            if (!message.envelope.is_final) return MessageHandlingResult::kInvalidPayload;
            auto result = result_identity(message.envelope);
            if (cancelled()) {
                result.status = VoiceStatus::kCancelled;
                result.detail = "work_cancelled";
                owner_.store_candidate(std::move(result));
                return MessageHandlingResult::kHandled;
            }
            v1::TextInput text;
            if (stage_ == Stage::kAsr) {
                if (message.envelope.topic != "cockpit.audio.input") return MessageHandlingResult::kUnsupportedTopic;
                v1::AudioInput audio;
                if (!audio.ParseFromString(message.payload) || audio.wav_bytes().empty()) return MessageHandlingResult::kInvalidPayload;
                const auto started_ns = owner_.clock.now_monotonic_ns();
                auto transcript = owner_.asr->transcribe(audio.wav_bytes(), cancelled);
                const auto finished_ns = owner_.clock.now_monotonic_ns();
                log_backend_call(message, transcript.error, finished_ns - started_ns);
                if (!transcript) { result.detail = transcript.detail; result.status = transcript.error == BackendError::kCancelled ? VoiceStatus::kCancelled : VoiceStatus::kFailed; }
                else if (transcript.value.empty() || transcript.value.size() > kMaxTextBytes) result.detail = "empty_or_oversized_asr_transcript";
                else text.set_text(std::move(transcript.value));
                if (text.text().empty()) { owner_.store_candidate(std::move(result)); return MessageHandlingResult::kHandled; }
            } else {
                if (message.envelope.topic != "cockpit.text.input") return MessageHandlingResult::kUnsupportedTopic;
                if (!text.ParseFromString(message.payload) || text.text().empty() || text.text().size() > kMaxTextBytes)
                    return MessageHandlingResult::kInvalidPayload;
            }
            result.transcript = text.text();
            std::uint64_t sequence = 0;
            auto make = [&](std::string_view topic, std::string_view target, std::string payload) {
                Message input;
                input.envelope = message.envelope;
                input.envelope.message_id += ":stage:" + std::to_string(sequence++);
                input.envelope.source_node = "voice.pipeline";
                input.envelope.target_node = std::string(target);
                input.envelope.topic = std::string(topic);
                input.envelope.sequence = 0;
                input.payload = std::move(payload);
                return input;
            };
            StageResult stage;
            if (stage_ == Stage::kAsr) {
                // ASR 根只等其他 Target；文本根则直接跑 Router，避免等待自己的 worker。
                stage = owner_.deliver(make("cockpit.router.input", "dialogue.primary", text.SerializeAsString()));
            } else {
                owner_.reset_stage();
                const auto handled = route(message);
                if (handled != MessageHandlingResult::kHandled) return handled;
                stage = owner_.take_stage();
            }
            auto fail = [&](const StageResult& failed) {
                result.status = failed.error == BackendError::kCancelled ? VoiceStatus::kCancelled : VoiceStatus::kFailed;
                result.detail = failed.detail;
                owner_.store_candidate(std::move(result));
                return MessageHandlingResult::kHandled;
            };
            if (!stage) return fail(stage);
            v1::TextOutput routed;
            if (!routed.ParseFromString(stage.value)) return fail({{}, BackendError::kInferenceFailed, "invalid_router_output"});
            if (routed.intent() == v1::COCKPIT_INTENT_UNRECOGNIZED) {
                stage = owner_.deliver(make("cockpit.llm.input", "llm.primary", text.SerializeAsString()));
                if (!stage) return fail(stage);
                v1::SpeechText answer;
                if (!answer.ParseFromString(stage.value) || answer.text().empty()) return fail({{}, BackendError::kInferenceFailed, "invalid_llm_output"});
                result.answer = answer.text();
            } else if (routed.intent() == v1::COCKPIT_INTENT_CLIMATE_ON ||
                       routed.intent() == v1::COCKPIT_INTENT_CLIMATE_OFF ||
                       routed.intent() == v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_OPEN ||
                       routed.intent() == v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_CLOSE) {
                if (cancelled()) return fail(cancelled_result());
                stage = owner_.apply_vehicle(routed.intent());
                if (!stage) return fail(stage);
                result.answer = std::move(stage.value);
            } else return fail({{}, BackendError::kInvalidInput, "unsupported_vehicle_intent"});
            v1::SpeechText speech;
            speech.set_text(result.answer);
            speech.set_request_message_id(message.envelope.message_id);
            stage = owner_.deliver(make("cockpit.speech.text", "tts.primary", speech.SerializeAsString()));
            if (!stage) return fail(stage);
            v1::AudioOutput audio;
            if (!audio.ParseFromString(stage.value) || audio.request_message_id() != result.request_message_id || audio.wav_bytes().empty())
                return fail({{}, BackendError::kInferenceFailed, "invalid_tts_output_identity"});
            result.audio.bytes = audio.wav_bytes();
            result.status = VoiceStatus::kCompleted;
            owner_.store_candidate(std::move(result));
            return MessageHandlingResult::kHandled;
        }

        State& owner_;
        Stage stage_;
        DialogueTextNode router_;
        runtime::NodeContext* context_{nullptr};
    };

    std::unique_ptr<inference::AsrBackend> asr;
    std::unique_ptr<inference::LlmBackend> llm;
    std::unique_ptr<inference::TtsBackend> tts;
    std::unique_ptr<FakeVehicle> vehicle;
    std::mutex run_mutex;
    std::mutex shutdown_mutex;
    std::mutex mutex;
    std::condition_variable changed;
    bool accepting{true};
    bool cancellation_requested{false};
    bool handler_finished{false};
    bool task_action_applied{false};
    std::optional<protocol::MessageEnvelope> active_request;
    std::optional<VoiceResult> candidate;
    std::optional<StageResult> stage_result;
    runtime::SteadyClock clock;
    transport::InMemoryTransport transport;
    observability::ConsoleLogger logger;
    runtime::UnitRegistry registry;
    gateway::ControlService control{registry};
    gateway::ControlEnvelopeValidator validator{clock};
    runtime::Runtime runtime{transport, clock, logger};

    State(std::unique_ptr<inference::AsrBackend> a, std::unique_ptr<inference::LlmBackend> l,
          std::unique_ptr<inference::TtsBackend> t, std::unique_ptr<FakeVehicle> v)
        : asr(std::move(a)), llm(std::move(l)), tts(std::move(t)), vehicle(std::move(v)) {
        if (!asr || !llm || !tts || !vehicle) throw std::invalid_argument("all model backends and explicit FakeVehicle are required");
        for (const auto* unit : {"asr.primary", "dialogue.primary", "llm.primary", "tts.primary"}) {
            if (!registry.register_unit({unit, {unit}, 1})) throw std::runtime_error("cannot register pipeline unit");
        }
        for (const auto stage : {Stage::kAsr, Stage::kDialogue, Stage::kLlm, Stage::kTts}) {
            if (runtime.add_target_node(std::make_unique<ModelNode>(*this, stage), 2) != runtime::RuntimeError::kNone)
                throw std::runtime_error("cannot register model target");
        }
        if (runtime.start() != runtime::RuntimeError::kNone) throw std::runtime_error("cannot start voice pipeline");
    }
    ~State() { shutdown(); }

    bool is_cancelled() { std::lock_guard<std::mutex> lock(mutex); return cancellation_requested; }
    bool is_root(const protocol::MessageEnvelope& envelope) {
        std::lock_guard<std::mutex> lock(mutex);
        return active_request && same_request(*active_request, envelope);
    }
    bool try_reserve(const Message& request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!accepting || active_request) return false;
        active_request = request.envelope;
        cancellation_requested = false;
        task_action_applied = false;
        candidate.reset();
        stage_result.reset();
        return true;
    }
    void rollback(const Message& request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active_request || !same_request(*active_request, request.envelope)) return;
        active_request.reset();
        candidate.reset();
        stage_result.reset();
        changed.notify_all();
    }
    void cancel(const protocol::MessageEnvelope& envelope) {
        std::lock_guard<std::mutex> lock(mutex);
        if (active_request && active_request->session_id == envelope.session_id &&
            active_request->work_id == envelope.work_id) cancellation_requested = true;
    }
    void store_candidate(VoiceResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        candidate = std::move(result);
    }
    StageResult apply_vehicle(v1::CockpitIntent intent) {
        std::lock_guard<std::mutex> lock(mutex);
        if (cancellation_requested) return cancelled_result();
        // 设置与 cancel/complete 共用锁；action_applied 记录已执行事实，取消或 TTS 失败不回滚。
        std::string_view reply;
        switch (intent) {
        case v1::COCKPIT_INTENT_CLIMATE_ON:
        case v1::COCKPIT_INTENT_CLIMATE_OFF: {
            const bool actual_on = vehicle->set_climate(intent == v1::COCKPIT_INTENT_CLIMATE_ON);
            reply = actual_on ? "模拟空调已打开。" : "模拟空调已关闭。";
            break;
        }
        case v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_OPEN:
        case v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_CLOSE: {
            const bool actual_open = vehicle->set_left_front_window(intent == v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_OPEN);
            reply = actual_open ? "模拟左前车窗已打开。" : "模拟左前车窗已关闭。";
            break;
        }
        default: return {{}, BackendError::kInvalidInput, "unsupported_vehicle_intent"};
        }
        task_action_applied = true;
        // 先记录动作，再分配回答文本；分配失败也不能把已发生动作标成未执行。
        return {std::string(reply), BackendError::kNone, {}};
    }
    void reset_stage() { std::lock_guard<std::mutex> lock(mutex); stage_result.reset(); handler_finished = false; }
    void publish(StageResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        stage_result = cancellation_requested ? cancelled_result() : std::move(result);
    }
    StageResult take_stage_locked() {
        if (cancellation_requested) return cancelled_result();
        if (!stage_result) return {{}, BackendError::kInferenceFailed, "target_completed_without_result"};
        auto result = std::move(*stage_result);
        stage_result.reset();
        return result;
    }
    StageResult take_stage() { std::lock_guard<std::mutex> lock(mutex); return take_stage_locked(); }
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
            [this](DeliveryFailure reason) { publish(delivery_failure(reason)); }, [this] {
                std::lock_guard<std::mutex> lock(mutex);
                handler_finished = true;
                changed.notify_all();
            });
        if (!admitted.admitted)
            return {{}, BackendError::kInferenceFailed, "runtime_admission=" + std::string(runtime::to_string(admitted.delivery_result))};
        std::unique_lock<std::mutex> lock(mutex);
        // publish 只交接数据；completion 才证明下游 handler 已返回，随后才能启动下一阶段。
        changed.wait(lock, [this] { return handler_finished; });
        return take_stage_locked();
    }

    gateway::DataTaskOutput complete(const Message& request, std::optional<DeliveryFailure> failure) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active_request || !same_request(*active_request, request.envelope)) return {};
        auto result = candidate ? std::move(*candidate) : result_identity(request.envelope);
        if (failure) {
            const auto failed = delivery_failure(*failure);
            result.status = failed.error == BackendError::kCancelled ? VoiceStatus::kCancelled : VoiceStatus::kFailed;
            result.detail = failed.detail;
        } else if (!candidate) result.detail = "root_completed_without_result";
        // cancel 与最终提交共用此锁：先到的取消压过候选，提交后的 Exit 不改旧结果。
        if (cancellation_requested) { result.status = VoiceStatus::kCancelled; result.detail = "work_cancelled"; }
        const auto work = registry.find_work(result.session_id, result.work_id);
        if (work && work.work.state != runtime::WorkState::kExited) {
            const auto exited = registry.exit_work(result.session_id, result.work_id);
            if (!exited) {
                bool exited_concurrently = false;
                if (exited.error == runtime::UnitRegistryError::kWorkNotRunning) {
                    // Gateway 的 Exit 可在两次 Registry 调用之间完成清理；只观察已 Exited 的同一 work，不重试。
                    const auto observed = registry.find_work(result.session_id, result.work_id);
                    exited_concurrently = observed && observed.work.state == runtime::WorkState::kExited;
                }
                if (!exited_concurrently) { result.status = VoiceStatus::kFailed; result.detail = "work_cleanup_failed"; }
            }
        }
        runtime.cancel_work(result.session_id, result.work_id);
        v1::VoiceTaskResult output;
        output.set_request_message_id(result.request_message_id);
        output.set_transcript(result.transcript);
        // 从独立模拟状态和本次动作事实构造回执，不能依赖可能被 SDK 异常替换的 candidate。
        auto* receipt = output.mutable_vehicle();
        receipt->set_simulated(true);
        receipt->set_climate_on(vehicle->climate_on());
        receipt->set_action_applied(task_action_applied);
        receipt->set_left_front_window_open(vehicle->left_front_window_open());
        gateway::DataTaskOutput wire;
        wire.topic = "cockpit.task.result";
        if (result.status == VoiceStatus::kCompleted) {
            output.set_answer(result.answer);
            output.mutable_audio()->set_request_message_id(result.request_message_id);
            output.mutable_audio()->set_wav_bytes(std::move(result.audio.bytes));
        } else if (result.status == VoiceStatus::kCancelled) {
            output.mutable_cancelled()->set_reason(result.detail);
            wire.kind = protocol::MessageKind::kError;
        } else {
            output.mutable_failed()->set_message(result.detail);
            wire.kind = protocol::MessageKind::kError;
        }
        wire.payload = output.SerializeAsString();
        if (!gateway::ControlGateway::data_output_fits(request.envelope, wire)) {
            output.clear_result();
            output.clear_answer();
            output.clear_transcript();
            output.mutable_failed()->set_message("output_frame_exceeds_limit");
            wire.kind = protocol::MessageKind::kError;
            wire.payload = output.SerializeAsString();
        }
        // 根 completion 在所有下游返回之后执行；释放槽位后调用方才发布唯一最终结果。
        candidate.reset();
        stage_result.reset();
        active_request.reset();
        changed.notify_all();
        return wire;
    }

    void shutdown() {
        std::lock_guard<std::mutex> stopping(shutdown_mutex);
        std::unique_lock<std::mutex> lock(mutex);
        accepting = false;
        if (active_request) {
            cancellation_requested = true;
            runtime.cancel_work(active_request->session_id, active_request->work_id);
        }
        // Runtime 保持运行直到根清理；提前 stop 会丢弃下游队列并让根永久等待 completion。
        changed.wait(lock, [this] { return !active_request; });
        lock.unlock();
        runtime.stop();
    }

    protocol::v1::ControlResponse command(Message envelope, const protocol::v1::ControlRequest& request) {
        envelope.envelope.target_node = "runtime.control";
        envelope.envelope.topic = "control.request";
        envelope.payload = request.SerializeAsString();
        auto checked = validator.validate(envelope);
        if (!checked || !checked.request) throw std::runtime_error("invalid_pipeline_control_envelope");
        auto response = control.handle(*checked.request).response;
        if (response.has_error()) throw std::runtime_error(response.error().message());
        return response;
    }
};

VoicePipeline::VoicePipeline(std::unique_ptr<inference::AsrBackend> asr,
                             std::unique_ptr<inference::LlmBackend> llm,
                             std::unique_ptr<inference::TtsBackend> tts,
                             std::unique_ptr<FakeVehicle> vehicle)
    : state_(std::make_unique<State>(std::move(asr), std::move(llm), std::move(tts), std::move(vehicle))) {}
VoicePipeline::~VoicePipeline() = default;
runtime::Runtime& VoicePipeline::runtime() noexcept { return state_->runtime; }
runtime::UnitRegistry& VoicePipeline::registry() noexcept { return state_->registry; }
const runtime::Clock& VoicePipeline::clock() const noexcept { return state_->clock; }
gateway::DataTaskHooks VoicePipeline::hooks() {
    return {[this](const Message& request) { return state_->try_reserve(request); },
            [this](const Message& request) { state_->rollback(request); },
            [this](const Message& request, std::optional<DeliveryFailure> failure) { return state_->complete(request, failure); },
            [this](const protocol::MessageEnvelope& envelope) { state_->cancel(envelope); }};
}
void VoicePipeline::shutdown() { state_->shutdown(); }
bool VoicePipeline::cancel() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->active_request) return false;
    state_->cancellation_requested = true;
    state_->runtime.cancel_work(state_->active_request->session_id, state_->active_request->work_id);
    return true;
}
VoiceResult VoicePipeline::run_text(std::string text) { return run(false, std::move(text)); }
VoiceResult VoicePipeline::run_wav(inference::WavAudio audio) { return run(true, std::move(audio.bytes)); }

VoiceResult VoicePipeline::run(bool audio_input, std::string input) {
    VoiceResult result;
    std::unique_lock<std::mutex> one_run(state_->run_mutex, std::try_to_lock);
    if (!one_run.owns_lock()) { result.status = VoiceStatus::kBusy; result.detail = "task_already_active"; return result; }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->active_request) { result.status = VoiceStatus::kBusy; result.detail = "task_already_active"; return result; }
        if (!state_->accepting) { result.detail = "pipeline_shutting_down"; return result; }
    }
    Message root;
    root.envelope.trace_id = new_trace_id();
    root.envelope.session_id = root.envelope.trace_id + ":session";
    root.envelope.message_id = root.envelope.trace_id + ":input";
    root.envelope.source_node = "voice.client";
    root.envelope.target_node = audio_input ? "asr.primary" : "dialogue.primary";
    root.envelope.topic = audio_input ? "cockpit.audio.input" : "cockpit.text.input";
    root.envelope.created_monotonic_ns = state_->clock.now_monotonic_ns();
    root.envelope.ttl_ms = kTaskTtlMs;
    root.envelope.is_final = true;
    bool slot_reserved = false;
    auto task_hooks = hooks();
    try {
        protocol::v1::ControlRequest setup;
        setup.mutable_setup()->set_unit_id(root.envelope.target_node);
        root.envelope.work_id = state_->command(root, setup).setup().work().work_id();
        result = result_identity(root.envelope);
        if (audio_input) { v1::AudioInput payload; payload.set_wav_bytes(std::move(input)); root.payload = payload.SerializeAsString(); }
        else { v1::TextInput payload; payload.set_text(std::move(input)); root.payload = payload.SerializeAsString(); }
        slot_reserved = task_hooks.try_reserve(root);
        if (!slot_reserved) { result.status = VoiceStatus::kBusy; throw std::runtime_error("task_already_active_or_shutting_down"); }
        const auto encoded = protocol::encode_message(root);
        if (!encoded || encoded.bytes.size() > kMaxFrameBytes) throw std::runtime_error("encoded_runtime_frame_exceeds_limit_or_invalid");
        auto reserved = state_->runtime.reserve_target(root.envelope.target_node);
        if (!reserved) throw std::runtime_error("root_target_reservation_failed");
        std::mutex completed_mutex;
        std::condition_variable completed_changed;
        bool completed = false;
        std::optional<DeliveryFailure> failure;
        gateway::DataTaskOutput output;
        const auto admitted = state_->runtime.admit_reserved(std::move(reserved.reservation), root,
            [&](DeliveryFailure reason) { failure = reason; }, [&] {
                auto terminal = task_hooks.complete(root, failure);
                std::lock_guard<std::mutex> lock(completed_mutex);
                output = std::move(terminal);
                completed = true;
                completed_changed.notify_one();
            });
        if (!admitted.admitted) throw std::runtime_error("root_admission=" + std::string(runtime::to_string(admitted.delivery_result)));
        {
            std::unique_lock<std::mutex> lock(completed_mutex);
            completed_changed.wait(lock, [&] { return completed; });
        }
        slot_reserved = false;
        v1::VoiceTaskResult terminal;
        if (!terminal.ParseFromString(output.payload) || terminal.request_message_id() != result.request_message_id)
            throw std::runtime_error("invalid_final_task_result");
        if (!terminal.has_vehicle() || !terminal.vehicle().simulated() || !terminal.vehicle().has_left_front_window_open())
            throw std::runtime_error("invalid_final_vehicle_receipt");
        result.vehicle = VehicleReceipt{terminal.vehicle().simulated(), terminal.vehicle().climate_on(),
                                        terminal.vehicle().action_applied(), terminal.vehicle().left_front_window_open()};
        result.transcript = terminal.transcript();
        if (terminal.has_audio()) { result.status = VoiceStatus::kCompleted; result.answer = terminal.answer(); result.audio.bytes = terminal.audio().wav_bytes(); }
        else if (terminal.has_cancelled()) { result.status = VoiceStatus::kCancelled; result.detail = terminal.cancelled().reason(); }
        else if (terminal.has_failed()) result.detail = terminal.failed().message();
        else throw std::runtime_error("final_task_result_missing_outcome");
    } catch (const std::exception& error) {
        result.detail = error.what();
        if (slot_reserved) task_hooks.rollback(root);
        if (!root.envelope.work_id.empty()) {
            const auto work = state_->registry.find_work(root.envelope.session_id, root.envelope.work_id);
            if (work && work.work.state != runtime::WorkState::kExited)
                static_cast<void>(state_->registry.exit_work(root.envelope.session_id, root.envelope.work_id));
            state_->runtime.cancel_work(root.envelope.session_id, root.envelope.work_id);
        }
    }
    return result;
}

}  // namespace cabinflow::agent
