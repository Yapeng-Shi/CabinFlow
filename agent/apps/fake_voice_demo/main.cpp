#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <cockpit_audio.pb.h>
#include <cockpit_text.pb.h>
#include <control.pb.h>

#include <cabinflow/agent/dialogue_text_node.hpp>
#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

namespace {

using cabinflow::protocol::Message;
using cabinflow::protocol::MessageEnvelope;
using cabinflow::runtime::MessageHandlingResult;
namespace agent_v1 = cabinflow::agent::v1;

constexpr std::uint32_t kSampleRate = 16'000;
constexpr std::uint32_t kFixtureSamples = 3'200;
constexpr std::uint32_t kResponseSamples = 6'400;
constexpr std::size_t kWavHeaderBytes = 44;
constexpr std::streamoff kMaxWavInputBytes = 4 * 1024 * 1024;
constexpr std::string_view kTranscript = "打开空调";
constexpr std::string_view kAnswer = "已识别打开空调意图，未执行车控。";

void put_u16(std::string& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void put_u32(std::string& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::string make_wav(std::uint32_t sample_count, bool tone) {
    const auto data_bytes = sample_count * 2U;
    std::string bytes;
    bytes.reserve(kWavHeaderBytes + data_bytes);
    bytes.append("RIFF", 4);
    put_u32(bytes, 36U + data_bytes);
    bytes.append("WAVEfmt ", 8);
    put_u32(bytes, 16U);
    put_u16(bytes, 1U);
    put_u16(bytes, 1U);
    put_u32(bytes, kSampleRate);
    put_u32(bytes, kSampleRate * 2U);
    put_u16(bytes, 2U);
    put_u16(bytes, 16U);
    bytes.append("data", 4);
    put_u32(bytes, data_bytes);
    for (std::uint32_t index = 0; index < sample_count; ++index) {
        // 测试音不是语音：整数方波保证同一输入在不同运行中产生相同字节。
        const std::int16_t sample = tone ? ((index / 20U) % 2U == 0U ? 2'000 : -2'000)
                                         : 0;
        put_u16(bytes, static_cast<std::uint16_t>(sample));
    }
    return bytes;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open input WAV: " + path);
    }
    const auto size = input.tellg();
    if (size < 0 || size > kMaxWavInputBytes) {
        throw std::runtime_error("input WAV exceeds readable size limit: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::string bytes((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("cannot read input WAV: " + path);
    }
    return bytes;
}

void write_file(const std::string& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot write WAV: " + path);
    }
}

struct StageOutput {
    MessageEnvelope source;
    std::string topic;
    std::string payload;
};

class StageSink final {
public:
    void emit(const MessageEnvelope& source, std::string_view topic,
              std::string payload) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            output_ = StageOutput{source, std::string(topic), std::move(payload)};
        }
        changed_.notify_one();
    }

    void fail(cabinflow::runtime::Runtime::TargetDeliveryFailure failure) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = failure;
        }
        changed_.notify_one();
    }

    StageOutput take() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [this] {
                return output_.has_value() || failure_.has_value();
            })) {
            throw std::runtime_error("fake stage timed out");
        }
        if (failure_) {
            std::string_view reason;
            switch (*failure_) {
                case cabinflow::runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded:
                    reason = "deadline_exceeded";
                    break;
                case cabinflow::runtime::Runtime::TargetDeliveryFailure::kSessionCancelled:
                    reason = "session_cancelled";
                    break;
                case cabinflow::runtime::Runtime::TargetDeliveryFailure::kWorkCancelled:
                    reason = "work_cancelled";
                    break;
                case cabinflow::runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic:
                    reason = "unsupported_topic";
                    break;
                case cabinflow::runtime::Runtime::TargetDeliveryFailure::kInvalidPayload:
                    reason = "invalid_payload";
                    break;
            }
            throw std::runtime_error("Runtime rejected fake stage: " +
                                     std::string(reason));
        }
        auto output = std::move(*output_);
        output_.reset();
        return output;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<StageOutput> output_;
    std::optional<cabinflow::runtime::Runtime::TargetDeliveryFailure> failure_;
};

class FakeAsrNode final : public cabinflow::runtime::TargetNode {
public:
    explicit FakeAsrNode(StageSink& sink) : sink_(sink) {}
    std::string_view name() const noexcept override { return "asr.primary"; }
    cabinflow::runtime::RuntimeError start(cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }
    void stop() noexcept override {}
    MessageHandlingResult on_message(const Message& message) noexcept override {
        if (message.envelope.topic != "cockpit.audio.input") {
            return MessageHandlingResult::kUnsupportedTopic;
        }
        agent_v1::AudioInput input;
        if (!input.ParseFromString(message.payload) ||
            input.wav_bytes() != make_wav(kFixtureSamples, false)) {
            return MessageHandlingResult::kInvalidPayload;
        }
        agent_v1::TextInput text;
        text.set_text(std::string(kTranscript));
        sink_.emit(message.envelope, "cockpit.text.input", text.SerializeAsString());
        return MessageHandlingResult::kHandled;
    }

private:
    StageSink& sink_;
};

class FakeLlmNode final : public cabinflow::runtime::TargetNode {
public:
    explicit FakeLlmNode(StageSink& sink) : sink_(sink) {}
    std::string_view name() const noexcept override { return "llm.fake"; }
    cabinflow::runtime::RuntimeError start(cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }
    void stop() noexcept override {}
    MessageHandlingResult on_message(const Message& message) noexcept override {
        if (message.envelope.topic != "cockpit.text.output") {
            return MessageHandlingResult::kUnsupportedTopic;
        }
        agent_v1::TextOutput routed;
        if (!routed.ParseFromString(message.payload) ||
            routed.intent() != agent_v1::COCKPIT_INTENT_CLIMATE_ON ||
            routed.request_message_id().empty()) {
            return MessageHandlingResult::kInvalidPayload;
        }
        agent_v1::SpeechText answer;
        answer.set_text(std::string(kAnswer));
        sink_.emit(message.envelope, "cockpit.speech.text", answer.SerializeAsString());
        return MessageHandlingResult::kHandled;
    }

private:
    StageSink& sink_;
};

class FakeTtsNode final : public cabinflow::runtime::TargetNode {
public:
    explicit FakeTtsNode(StageSink& sink) : sink_(sink) {}
    std::string_view name() const noexcept override { return "tts.fake"; }
    cabinflow::runtime::RuntimeError start(cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }
    void stop() noexcept override {}
    MessageHandlingResult on_message(const Message& message) noexcept override {
        if (message.envelope.topic != "cockpit.speech.text") {
            return MessageHandlingResult::kUnsupportedTopic;
        }
        agent_v1::SpeechText text;
        if (!text.ParseFromString(message.payload) || text.text() != kAnswer ||
            text.request_message_id().empty()) {
            return MessageHandlingResult::kInvalidPayload;
        }
        agent_v1::AudioOutput output;
        output.set_request_message_id(text.request_message_id());
        output.set_wav_bytes(make_wav(kResponseSamples, true));
        sink_.emit(message.envelope, "cockpit.audio.output", output.SerializeAsString());
        return MessageHandlingResult::kHandled;
    }

private:
    StageSink& sink_;
};

Message make_message(const StageOutput& previous, std::string id,
                     std::string_view target,
                     cabinflow::runtime::Clock& clock) {
    Message message;
    message.envelope = previous.source;
    message.envelope.message_id = std::move(id);
    message.envelope.source_node = previous.source.target_node;
    message.envelope.target_node = std::string(target);
    message.envelope.topic = previous.topic;
    message.envelope.sequence = 0;  // 每个业务 topic 是独立流，首条消息从 0 开始。
    message.envelope.created_monotonic_ns = clock.now_monotonic_ns();
    message.envelope.is_final = true;
    message.payload = previous.payload;
    return message;
}

void deliver(cabinflow::runtime::Runtime& runtime, StageSink& sink,
             Message message) {
    const auto target = message.envelope.target_node;
    auto reservation = runtime.reserve_target(target);
    if (!reservation) {
        throw std::runtime_error("no capacity for target: " + target);
    }
    const auto admitted = runtime.admit_reserved(
        std::move(reservation.reservation), std::move(message),
        [&sink](auto failure) { sink.fail(failure); });
    if (!admitted.admitted) {
        throw std::runtime_error("Runtime admission failed: " +
                                 std::string(cabinflow::runtime::to_string(
                                     admitted.delivery_result)));
    }
}

void log_stage(std::string_view stage, const StageOutput& output) {
    std::cout << "stage=" << stage << " trace=" << output.source.trace_id
              << " session=" << output.source.session_id
              << " work=" << output.source.work_id
              << " message=" << output.source.message_id
              << " output_topic=" << output.topic << '\n';
}

cabinflow::protocol::v1::ControlResponse send_control(
    cabinflow::gateway::ControlEnvelopeValidator& validator,
    cabinflow::gateway::ControlService& service,
    cabinflow::runtime::Clock& clock,
    std::string message_id, std::string session_id, std::string work_id,
    const cabinflow::protocol::v1::ControlRequest& command) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "fake-voice-trace";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "fake-voice-cli";
    message.envelope.target_node = "runtime.control";
    message.envelope.topic = "control.request";
    message.envelope.created_monotonic_ns = clock.now_monotonic_ns();
    message.envelope.ttl_ms = 10'000;
    message.envelope.is_final = true;
    if (!command.SerializeToString(&message.payload)) {
        throw std::runtime_error("cannot encode fake control request");
    }
    const auto validated = validator.validate(message);
    if (!validated || !validated.request) {
        throw std::runtime_error("fake control request failed envelope validation");
    }
    auto response = service.handle(*validated.request).response;
    if (response.has_error()) {
        throw std::runtime_error("fake control request failed: " +
                                 response.error().message());
    }
    return response;
}

void run_pipeline(const std::string& input_path, const std::string& output_path) {
    StageSink sink;
    cabinflow::runtime::SteadyClock clock;
    cabinflow::transport::InMemoryTransport transport;
    cabinflow::observability::ConsoleLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);
    cabinflow::runtime::UnitRegistry registry;
    cabinflow::gateway::ControlEnvelopeValidator validator(clock);
    cabinflow::gateway::ControlService control(registry);
    cabinflow::protocol::v1::ControlRequest command;
    auto* registration = command.mutable_register_unit();
    registration->set_unit_id("asr.primary");
    registration->add_capabilities("offline.asr.fake");
    registration->set_max_concurrent_work(1);
    const auto registered = send_control(validator, control, clock,
                                         "fake-register-1", "", "", command);
    if (!registered.has_register_unit()) {
        throw std::runtime_error("fake ASR unit registration did not succeed");
    }
    command.Clear();
    command.mutable_setup()->set_unit_id("asr.primary");
    const auto setup = send_control(validator, control, clock,
                                    "fake-setup-1", "fake-session", "", command);
    if (!setup.has_setup() || setup.setup().work().work_id().empty()) {
        throw std::runtime_error("fake voice setup did not return a work id");
    }
    const auto work_id = setup.setup().work().work_id();
    if (runtime.add_target_node(std::make_unique<FakeAsrNode>(sink), 2) !=
            cabinflow::runtime::RuntimeError::kNone ||
        runtime.add_target_node(
            std::make_unique<cabinflow::agent::DialogueTextNode>(
                [&sink](const MessageEnvelope& envelope, std::string_view topic,
                        std::string payload) {
                    sink.emit(envelope, topic, std::move(payload));
                    return true;
                }), 2) != cabinflow::runtime::RuntimeError::kNone ||
        runtime.add_target_node(std::make_unique<FakeLlmNode>(sink), 2) !=
            cabinflow::runtime::RuntimeError::kNone ||
        runtime.add_target_node(std::make_unique<FakeTtsNode>(sink), 2) !=
            cabinflow::runtime::RuntimeError::kNone ||
        runtime.start() != cabinflow::runtime::RuntimeError::kNone) {
        throw std::runtime_error("cannot start fake voice targets");
    }

    agent_v1::AudioInput audio;
    audio.set_wav_bytes(read_file(input_path));
    Message first;
    first.envelope.message_id = "fake-audio-1";
    first.envelope.trace_id = "fake-voice-trace";
    first.envelope.session_id = "fake-session";
    first.envelope.work_id = work_id;
    first.envelope.source_node = "fake-voice-cli";
    first.envelope.target_node = "asr.primary";
    first.envelope.topic = "cockpit.audio.input";
    first.envelope.created_monotonic_ns = clock.now_monotonic_ns();
    first.envelope.ttl_ms = 10'000;
    first.envelope.is_final = true;
    first.payload = audio.SerializeAsString();
    const auto audio_request_id = first.envelope.message_id;
    const auto audio_request_envelope = first.envelope;

    deliver(runtime, sink, std::move(first));
    auto stage = sink.take();
    log_stage("fake_asr", stage);
    deliver(runtime, sink, make_message(stage, "fake-text-1", "dialogue.primary", clock));
    stage = sink.take();
    log_stage("router", stage);
    deliver(runtime, sink, make_message(stage, "fake-llm-1", "llm.fake", clock));
    stage = sink.take();
    log_stage("fake_llm", stage);
    agent_v1::SpeechText speech;
    if (!speech.ParseFromString(stage.payload) || speech.text().empty()) {
        throw std::runtime_error("fake LLM returned invalid speech text");
    }
    // 编排层保留原始音频请求关联；TextOutput 的 request_message_id 只关联直接文本输入。
    speech.set_request_message_id(audio_request_id);
    stage.payload = speech.SerializeAsString();
    deliver(runtime, sink, make_message(stage, "fake-tts-1", "tts.fake", clock));
    stage = sink.take();
    log_stage("fake_tts", stage);
    if (stage.topic != "cockpit.audio.output") {
        throw std::runtime_error("fake TTS returned wrong topic");
    }
    agent_v1::AudioOutput output;
    if (!output.ParseFromString(stage.payload) ||
        output.request_message_id() != audio_request_id ||
        output.wav_bytes() != make_wav(kResponseSamples, true)) {
        throw std::runtime_error("fake TTS output contract failed");
    }
    write_file(output_path, output.wav_bytes());
    command.Clear();
    command.mutable_exit()->set_reason("fake voice demo completed");
    const auto exited = send_control(validator, control, clock,
                                     "fake-exit-1", "fake-session", work_id, command);
    if (!exited.has_exit() || exited.exit().work().work_id() != work_id) {
        throw std::runtime_error("cannot exit fake voice work");
    }
    // CLI 模拟 Gateway 的 Exit 副作用：ControlService 改状态，Runtime 取消未开始工作。
    runtime.cancel_work("fake-session", work_id);
    Message after_exit;
    after_exit.envelope = audio_request_envelope;
    after_exit.envelope.message_id = "fake-after-exit";
    after_exit.envelope.sequence = 1;
    after_exit.envelope.created_monotonic_ns = clock.now_monotonic_ns();
    after_exit.envelope.is_final = false;
    after_exit.payload = audio.SerializeAsString();
    auto after_exit_slot = runtime.reserve_target("asr.primary");
    if (!after_exit_slot) {
        throw std::runtime_error("cannot reserve post-exit admission check");
    }
    const auto after_exit_result = runtime.admit_reserved(
        std::move(after_exit_slot.reservation), std::move(after_exit), {});
    if (after_exit_result.admitted ||
        after_exit_result.delivery_result !=
            cabinflow::runtime::DeliveryResult::kWorkCancelled) {
        throw std::runtime_error("post-exit data was not rejected as work_cancelled");
    }
    std::cout << "state_after_exit=work_cancelled trace=fake-voice-trace"
              << " session=fake-session work=" << work_id << '\n';
    runtime.stop();
    std::cout << "status=completed output=" << output_path
              << " request_message_id=" << output.request_message_id()
              << " audio=fake_test_tone_not_speech\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
            write_file(argv[2], make_wav(kFixtureSamples, false));
            return 0;
        }
        if (argc != 3) {
            throw std::runtime_error(
                "usage: fake_voice_demo --write-fixture INPUT.wav | "
                "fake_voice_demo INPUT.wav OUTPUT.wav");
        }
        run_pipeline(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fake_voice_demo: " << error.what() << '\n';
        return 1;
    }
}
