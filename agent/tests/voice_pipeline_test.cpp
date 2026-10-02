#include <cabinflow/agent/voice_pipeline.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace cabinflow::agent;
using namespace cabinflow::agent::inference;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Evidence {
    std::atomic<int> asr_calls{0}, llm_calls{0}, tts_calls{0};
    std::string llm_input, tts_input;
    bool fail_llm{false};
    bool block_llm{false};
    bool block_tts{false};
    bool oversized_audio{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, released{false};
};

class TestAsr final : public AsrBackend {
public:
    explicit TestAsr(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<std::string> transcribe(std::string_view, const CancellationCheck&) override {
        ++evidence_.asr_calls;
        return {"法国的首都是什么", BackendError::kNone, {}};
    }
private:
    Evidence& evidence_;
};

class TestLlm final : public LlmBackend {
public:
    explicit TestLlm(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<std::string> generate(std::string_view text, const CancellationCheck&) override {
        ++evidence_.llm_calls;
        evidence_.llm_input = text;
        if (evidence_.block_llm) {
            std::unique_lock<std::mutex> lock(evidence_.mutex);
            evidence_.entered = true;
            evidence_.changed.notify_one();
            // 故意模拟不可硬中断的 SDK：返回后编排必须丢弃结果，不能提前声称已退出。
            evidence_.changed.wait(lock, [this] { return evidence_.released; });
        }
        if (evidence_.fail_llm) return {{}, BackendError::kInferenceFailed, "injected_sdk_failure"};
        return {"模型结果：" + std::string(text), BackendError::kNone, {}};
    }
private:
    Evidence& evidence_;
};

class TestTts final : public TtsBackend {
public:
    explicit TestTts(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<WavAudio> synthesize(std::string_view text, const CancellationCheck&) override {
        ++evidence_.tts_calls;
        evidence_.tts_input = text;
        if (evidence_.block_tts) {
            std::unique_lock<std::mutex> lock(evidence_.mutex);
            evidence_.entered = true;
            evidence_.changed.notify_one();
            // 最终 SDK 即使晚到成功，取消也必须胜出，不能把旧音频当新结果。
            evidence_.changed.wait(lock, [this] { return evidence_.released; });
        }
        if (evidence_.oversized_audio)
            return {{std::string(4 * 1024 * 1024, 'x')}, BackendError::kNone, {}};
        // 仅验证消息与端口所有权；这不是可听语音或真实 TTS 验收。
        return {{"deterministic-test-audio"}, BackendError::kNone, {}};
    }
private:
    Evidence& evidence_;
};

void text_and_failure_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence));
    auto first = pipeline.run_text("法国的首都是什么");
    require(first.status == VoiceStatus::kCompleted, "text execution completed");
    require(evidence.asr_calls == 0, "explicit text mode skips ASR");
    require(evidence.llm_input == "法国的首都是什么", "LLM receives original question, not Router receipt");
    require(evidence.tts_input == first.answer, "TTS receives actual LLM output");
    require(!first.trace_id.empty() && !first.work_id.empty() && !first.request_message_id.empty(), "identities retained");
    const auto original_tts_calls = evidence.tts_calls.load();
    evidence.fail_llm = true;
    auto failed = pipeline.run_text("另一条问题");
    require(failed.status == VoiceStatus::kFailed && failed.detail == "injected_sdk_failure", "SDK failure propagated");
    require(failed.audio.bytes.empty() && failed.answer.empty(), "failure publishes no old result");
    require(evidence.tts_calls == original_tts_calls, "failure stops downstream TTS");
    require(failed.work_id != first.work_id, "every new task has a new work");
    evidence.fail_llm = false;
    auto intent = pipeline.run_text("打开空调");
    require(intent.status == VoiceStatus::kCompleted && intent.answer.find("未执行车控") != std::string::npos,
            "explicit intent keeps unexecuted receipt truthful");
    require(evidence.llm_calls == 2, "recognized intent does not ask LLM to invent execution");
    auto audio = pipeline.run_wav({"test-input-not-real-wav"});
    require(audio.status == VoiceStatus::kCompleted && evidence.asr_calls == 1, "WAV entry uses ASR port");
    require(audio.transcript == "法国的首都是什么" && evidence.llm_input == audio.transcript,
            "ASR transcript routed unchanged to LLM");
    require(pipeline.run_text("").status == VoiceStatus::kFailed, "empty input rejected");
    require(!pipeline.cancel(), "idle cancellation is not accepted");
}

void cancellation_contract(bool during_tts) {
    Evidence evidence;
    evidence.block_llm = !during_tts;
    evidence.block_tts = during_tts;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence));
    VoiceResult result;
    std::atomic<bool> returned{false};
    std::thread controller([&] { result = pipeline.run_text("正在推理的问题"); returned = true; });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(evidence.mutex);
        entered = evidence.changed.wait_for(lock, std::chrono::seconds(2), [&] { return evidence.entered; });
    }
    const bool cancel_accepted = entered && pipeline.cancel();
    const bool returned_before_release = returned.load();
    const auto busy = pipeline.run_text("不能并发接受的新任务");
    {
        std::lock_guard<std::mutex> lock(evidence.mutex);
        evidence.released = true;
    }
    evidence.changed.notify_one();
    controller.join();
    require(entered && cancel_accepted, "cancellation occurs inside SDK barrier");
    require(!returned_before_release, "cancel request does not pretend handler has exited");
    require(busy.status == VoiceStatus::kBusy, "one active task during cancellation cleanup");
    require(result.status == VoiceStatus::kCancelled && result.audio.bytes.empty() && result.answer.empty(),
            "late output suppressed and terminal state cancelled");
    require(evidence.tts_calls == (during_tts ? 1 : 0), "cancellation starts no extra downstream handler");
    evidence.block_llm = false;
    evidence.block_tts = false;
    const auto next = pipeline.run_text("清理之后的新任务");
    require(next.status == VoiceStatus::kCompleted && next.work_id != result.work_id,
            "next task starts only after old handler completion with fresh work");
}

void frame_boundary_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence));
    // 字节体本身尚未到上限，但加入 Protobuf 字段和 Envelope 后必定超限。
    const auto rejected = pipeline.run_wav({std::string(4 * 1024 * 1024 - 1, 'x')});
    require(rejected.status == VoiceStatus::kFailed && evidence.asr_calls == 0,
            "full encoded input frame limit checked before ASR");
    evidence.oversized_audio = true;
    const auto output = pipeline.run_text("输出边界问题");
    require(output.status == VoiceStatus::kFailed && output.audio.bytes.empty() && output.answer.empty(),
            "oversized final output never published as successful audio");
}
}

int main() {
    try {
        text_and_failure_contract();
        cancellation_contract(false);
        cancellation_contract(true);
        frame_boundary_contract();
        std::cout << "voice pipeline contracts passed; test backends are not real inference\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
