#include <cabinflow/agent/voice_pipeline.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/unit_registry.hpp>

#include <cockpit_task.pb.h>
#include <cockpit_text.pb.h>

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
    bool block_asr{false};
    bool block_llm{false};
    bool block_tts{false};
    bool oversized_audio{false};
    bool fail_tts{false};
    bool throw_tts{false};
    std::string llm_reply;
    std::string asr_transcript{"法国的首都是什么"};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, released{false};
};

class TestAsr final : public AsrBackend {
public:
    explicit TestAsr(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<std::string> transcribe(std::string_view, const CancellationCheck&) override {
        ++evidence_.asr_calls;
        if (evidence_.block_asr) {
            std::unique_lock<std::mutex> lock(evidence_.mutex);
            evidence_.entered = true;
            evidence_.changed.notify_one();
            evidence_.changed.wait(lock, [this] { return evidence_.released; });
        }
        return {evidence_.asr_transcript, BackendError::kNone, {}};
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
        if (!evidence_.llm_reply.empty()) return {evidence_.llm_reply, BackendError::kNone, {}};
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
        if (evidence_.throw_tts) throw std::runtime_error("injected_tts_exception");
        if (evidence_.fail_tts) return {{}, BackendError::kInferenceFailed, "injected_tts_failure"};
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
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
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
    require(intent.status == VoiceStatus::kCompleted && intent.answer == "模拟空调已打开。" &&
                intent.vehicle && intent.vehicle->simulated && intent.vehicle->climate_on && intent.vehicle->action_applied,
            "explicit climate intent reports actual FakeVehicle execution");
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
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
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
    require(busy.status == VoiceStatus::kBusy && !busy.vehicle, "busy request has no invented vehicle observation");
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
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    // 字节体本身尚未到上限，但加入 Protobuf 字段和 Envelope 后必定超限。
    const auto rejected = pipeline.run_wav({std::string(4 * 1024 * 1024 - 1, 'x')});
    require(rejected.status == VoiceStatus::kFailed && evidence.asr_calls == 0,
            "full encoded input frame limit checked before ASR");
    evidence.oversized_audio = true;
    const auto output = pipeline.run_text("输出边界问题");
    require(output.status == VoiceStatus::kFailed && output.audio.bytes.empty() && output.answer.empty(),
            "oversized final output never published as successful audio");
}

cabinflow::protocol::Message make_root(VoicePipeline& pipeline, std::string id, std::string payload) {
    cabinflow::protocol::Message request;
    request.envelope.trace_id = id;
    request.envelope.session_id = id + ":session";
    request.envelope.message_id = id + ":request";
    request.envelope.source_node = "test.client";
    request.envelope.target_node = "dialogue.primary";
    request.envelope.topic = "cockpit.text.input";
    request.envelope.is_final = true;
    request.envelope.created_monotonic_ns = pipeline.clock().now_monotonic_ns();
    request.envelope.ttl_ms = 300'000;
    const auto work = pipeline.registry().create_work(request.envelope.session_id, "dialogue.primary");
    require(static_cast<bool>(work), "test creates registered root work");
    request.envelope.work_id = work.work.work_id;
    request.payload = std::move(payload);
    return request;
}

std::string question_payload() {
    v1::TextInput input;
    input.set_text("候选结果测试问题");
    return input.SerializeAsString();
}

struct RootCompletion {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false}, released{false}, completed{false};
    bool pause_before_commit{false};
    bool pause_after_commit{false};
    std::optional<cabinflow::runtime::Runtime::TargetDeliveryFailure> failure;
    cabinflow::gateway::DataTaskOutput output;
    int calls{0};
};

struct RootCompletionGate {
    std::shared_ptr<RootCompletion> state{std::make_shared<RootCompletion>()};
    RootCompletionGate() = default;
    RootCompletionGate(const RootCompletionGate&) = delete;
    RootCompletionGate& operator=(const RootCompletionGate&) = delete;
    ~RootCompletionGate() {
        // 断言/准入异常也必须放行屏障；回调持有共享状态，不借用此栈对象或其 condition variable。
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
        state->changed.notify_all();
    }
    RootCompletion* operator->() const noexcept { return state.get(); }
};

void admit_root(VoicePipeline& pipeline, const cabinflow::protocol::Message& root, const RootCompletionGate& gate) {
    const auto hooks = pipeline.hooks();
    require(hooks.try_reserve(root), "application root slot reserved before Runtime admission");
    auto target = pipeline.runtime().reserve_target(root.envelope.target_node);
    if (!target) {
        hooks.rollback(root);
        throw std::runtime_error("test root Target reservation failed");
    }
    const auto completion = gate.state;
    const auto admitted = pipeline.runtime().admit_reserved(std::move(target.reservation), root,
        [completion](auto failure) { completion->failure = failure; },
        [hooks, root, completion] {
            {
                std::unique_lock<std::mutex> lock(completion->mutex);
                completion->entered = true;
                completion->changed.notify_all();
                if (completion->pause_before_commit)
                    completion->changed.wait(lock, [&completion] { return completion->released; });
            }
            auto output = hooks.complete(root, completion->failure);
            {
                std::unique_lock<std::mutex> lock(completion->mutex);
                completion->output = std::move(output);
                ++completion->calls;
                completion->completed = true;
                completion->changed.notify_all();
                if (completion->pause_after_commit)
                    completion->changed.wait(lock, [&completion] { return completion->released; });
            }
        });
    if (!admitted.admitted) {
        hooks.rollback(root);
        throw std::runtime_error("test root Runtime admission failed");
    }
}

bool wait_completion(const RootCompletionGate& gate, bool before_commit = false) {
    const auto completion = gate.state;
    std::unique_lock<std::mutex> lock(completion->mutex);
    return completion->changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return before_commit ? completion->entered : completion->completed;
    });
}

void candidate_commit_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    v1::TextInput climate;
    climate.set_text("打开空调");
    auto root = make_root(pipeline, "candidate", climate.SerializeAsString());
    RootCompletionGate completion;
    completion->pause_before_commit = true;
    admit_root(pipeline, root, completion);
    const bool candidate_ready = wait_completion(completion, true);
    const bool cancel_accepted = candidate_ready && pipeline.cancel();
    const auto busy = pipeline.run_text("completion 之前的新输入");
    {
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->released = true;
    }
    completion->changed.notify_all();
    require(wait_completion(completion), "root completion finishes after release");
    require(candidate_ready && cancel_accepted && busy.status == VoiceStatus::kBusy,
            "candidate is not committed output and active slot retained until completion");
    v1::VoiceTaskResult result;
    require(result.ParseFromString(completion->output.payload) && result.has_cancelled() && !result.has_audio(),
            "cancel after candidate and before complete wins over successful SDK output");
    require(result.has_vehicle() && result.vehicle().simulated() && result.vehicle().climate_on() &&
                result.vehicle().action_applied(),
            "cancel after candidate preserves the already applied vehicle action");
    require(completion->calls == 1 && completion->output.kind == cabinflow::protocol::MessageKind::kError,
            "only one typed final cancelled result is committed");
    require(pipeline.registry().find_work(root.envelope.session_id, root.envelope.work_id).work.state ==
                cabinflow::runtime::WorkState::kExited,
            "work cleanup precedes final output");
    pipeline.hooks().cancel(root.envelope);
    require(!pipeline.cancel(), "late cancel cannot modify committed terminal state");
    require(pipeline.run_text("清理后的问题").status == VoiceStatus::kCompleted,
            "fresh root admitted after final cleanup");
}

void root_not_handled_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    for (const auto failure : {cabinflow::runtime::Runtime::TargetDeliveryFailure::kWorkCancelled,
                               cabinflow::runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded}) {
        // 前一根已 complete 并释放全局槽位，但 completion wrapper 仍挡住同一 worker。
        // 后一根确定排队；取消/过期后放行，从而实际经过 Runtime 的 handler 前检查。
        auto previous = make_root(pipeline, "blocking-previous-" + std::to_string(static_cast<int>(failure)), question_payload());
        RootCompletionGate blocker;
        blocker->pause_after_commit = true;
        admit_root(pipeline, previous, blocker);
        require(wait_completion(blocker), "previous terminal committed while worker remains gated");
        const auto llm_calls_before = evidence.llm_calls.load();
        const auto tts_calls_before = evidence.tts_calls.load();
        auto root = make_root(pipeline, "not-handled-" + std::to_string(static_cast<int>(failure)), question_payload());
        v1::TextInput climate;
        climate.set_text("打开空调");
        root.payload = climate.SerializeAsString();
        if (failure == cabinflow::runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded)
            root.envelope.ttl_ms = 100;
        RootCompletionGate completion;
        admit_root(pipeline, root, completion);
        bool cancel_accepted = true;
        if (failure == cabinflow::runtime::Runtime::TargetDeliveryFailure::kWorkCancelled)
            cancel_accepted = pipeline.cancel();
        else {
            const auto deadline = root.envelope.created_monotonic_ns +
                                  static_cast<std::uint64_t>(root.envelope.ttl_ms) * 1'000'000;
            while (pipeline.clock().now_monotonic_ns() < deadline) std::this_thread::yield();
        }
        {
            std::lock_guard<std::mutex> lock(blocker->mutex);
            blocker->released = true;
        }
        blocker->changed.notify_all();
        require(wait_completion(completion) && cancel_accepted, "queued root finalizes after cancellation/deadline");
        v1::VoiceTaskResult result;
        require(result.ParseFromString(completion->output.payload) && result.request_message_id() == root.envelope.message_id,
                "pre-handler failure has correlated final result");
        require(failure == cabinflow::runtime::Runtime::TargetDeliveryFailure::kWorkCancelled ? result.has_cancelled() : result.has_failed(),
                "pre-handler cancellation and deadline have explicit outcomes");
        require(result.has_vehicle() && result.vehicle().simulated() &&
                    !result.vehicle().climate_on() && !result.vehicle().action_applied(),
                "queued cancelled/expired climate command preserves initialOff without executing action");
        require(pipeline.registry().find_work(root.envelope.session_id, root.envelope.work_id).work.state ==
                    cabinflow::runtime::WorkState::kExited,
                "unhandled root exits work and releases application slot");
        require(evidence.llm_calls == llm_calls_before && evidence.tts_calls == tts_calls_before,
                "queued cancelled/expired root never enters model handler");
    }
    require(evidence.asr_calls == 0, "text roots remain explicit text mode");
    require(pipeline.run_text("前置失败清理后").status == VoiceStatus::kCompleted,
            "root failure cleanup permits next real handler");
}

void invalid_business_and_rollback_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    auto root = make_root(pipeline, "invalid-business", std::string("\xff", 1));
    RootCompletionGate completion;
    admit_root(pipeline, root, completion);
    require(wait_completion(completion), "invalid business payload completes root");
    v1::VoiceTaskResult failed;
    require(failed.ParseFromString(completion->output.payload) && failed.has_failed() && completion->calls == 1,
            "invalid business payload gets one final failure, no partial success");
    require(evidence.llm_calls == 0 && evidence.tts_calls == 0, "invalid input invokes no inference downstream");
    require(pipeline.registry().find_work(root.envelope.session_id, root.envelope.work_id).work.state ==
                cabinflow::runtime::WorkState::kExited,
            "invalid business work is ended instead of retrying or undoing Ledger");
    auto reserved = make_root(pipeline, "rollback-owner", question_payload());
    auto hooks = pipeline.hooks();
    require(hooks.try_reserve(reserved), "rollback test owns single active slot");
    auto other = reserved;
    other.envelope.message_id += ":other";
    hooks.rollback(other);
    require(!hooks.try_reserve(other), "unrelated rollback does not release active root");
    hooks.rollback(reserved);
    require(pipeline.registry().find_work(reserved.envelope.session_id, reserved.envelope.work_id).work.state ==
                cabinflow::runtime::WorkState::kRunning,
            "preadmission rollback releases only slot and does not mutate Registry work");
    static_cast<void>(pipeline.registry().exit_work(reserved.envelope.session_id, reserved.envelope.work_id));
    require(pipeline.run_text("非法输入清理后的新任务").status == VoiceStatus::kCompleted,
            "invalid business final and rollback preserve later admission");
}

void concurrent_exit_and_completion_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto root = make_root(pipeline, "concurrent-exit-" + std::to_string(attempt), question_payload());
        RootCompletionGate completion;
        completion->pause_before_commit = true;
        admit_root(pipeline, root, completion);
        const bool candidate_ready = wait_completion(completion, true);
        pipeline.hooks().cancel(root.envelope);
        cabinflow::runtime::WorkResult exit_result;
        // 同一屏障放行外部 Exit 和根 complete；这是有界竞争检查，不声称穷尽 Registry 锁的交错。
        std::thread exiting([&pipeline, &exit_result, root, state = completion.state] {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->changed.wait(lock, [&state] { return state->released; });
            }
            exit_result = pipeline.registry().exit_work(root.envelope.session_id, root.envelope.work_id);
        });
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->released = true;
        }
        completion->changed.notify_all();
        const bool finalized = wait_completion(completion);
        exiting.join();
        require(candidate_ready && finalized, "concurrent Exit and root completion both finish");
        require(exit_result.error == cabinflow::runtime::UnitRegistryError::kNone ||
                    exit_result.error == cabinflow::runtime::UnitRegistryError::kWorkNotRunning,
                "external Exit either cleans work or observes completion cleanup");
        v1::VoiceTaskResult result;
        require(result.ParseFromString(completion->output.payload) && result.has_cancelled(),
                "concurrent cleanup cannot overwrite accepted cancellation with failure");
        require(completion->calls == 1 && completion->output.kind == cabinflow::protocol::MessageKind::kError,
                "concurrent Exit preserves one ERROR terminal cancellation");
    }
}

void gate_exception_cleanup_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    const auto root = make_root(pipeline, "gate-exception", question_payload());
    std::shared_ptr<RootCompletion> observed;
    struct InjectedTestFailure {};
    bool injected = false;
    try {
        RootCompletionGate completion;
        observed = completion.state;
        completion->pause_before_commit = true;
        admit_root(pipeline, root, completion);
        require(wait_completion(completion, true), "exception test reaches root completion gate");
        throw InjectedTestFailure{};
    } catch (const InjectedTestFailure&) {
        injected = true;
    }
    std::unique_lock<std::mutex> lock(observed->mutex);
    const bool finished = observed->changed.wait_for(lock, std::chrono::seconds(2), [&] { return observed->completed; });
    require(injected && finished && observed->calls == 1,
            "stack unwinding releases completion gate without deadlocking Pipeline destruction");
}

void shutdown_waits_for_downstream_contract() {
    Evidence evidence;
    evidence.block_llm = true;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    VoiceResult result;
    std::atomic<bool> run_returned{false}, shutdown_returned{false};
    std::thread runner([&] { result = pipeline.run_wav({"test-input"}); run_returned = true; });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(evidence.mutex);
        entered = evidence.changed.wait_for(lock, std::chrono::seconds(2), [&] { return evidence.entered; });
    }
    // backend 尚未释放，根 ASR worker 正在等独立 LLM worker 的 completion。
    const auto cancel_count = pipeline.runtime().metric_value("runtime.work_cancelled");
    std::thread stopping([&] { pipeline.shutdown(); shutdown_returned = true; });
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pipeline.runtime().metric_value("runtime.work_cancelled") == cancel_count &&
           std::chrono::steady_clock::now() < stop_deadline) std::this_thread::yield();
    const bool shutdown_cancelled = pipeline.runtime().metric_value("runtime.work_cancelled") > cancel_count;
    const bool runtime_running_during_drain = pipeline.runtime().running();
    const bool premature_run = run_returned.load();
    const bool premature_shutdown = shutdown_returned.load();
    {
        std::lock_guard<std::mutex> lock(evidence.mutex);
        evidence.released = true;
    }
    evidence.changed.notify_one();
    runner.join();
    stopping.join();
    require(entered && shutdown_cancelled && runtime_running_during_drain &&
                !premature_run && !premature_shutdown && shutdown_returned && run_returned,
            "shutdown drains root and synchronous SDK before stop returns");
    require(result.status == VoiceStatus::kCancelled && result.audio.bytes.empty(),
            "shutdown cancellation suppresses downstream output");
    require(!pipeline.runtime().running(), "Runtime stops only after active root finalized");
    const auto after_shutdown = pipeline.run_text("shutdown 后");
    require(after_shutdown.status == VoiceStatus::kFailed && !after_shutdown.vehicle,
            "shutdown disables fresh root admission");
}

void require_vehicle(const VoiceResult& result, bool climate_on, bool action_applied, bool window_open) {
    require(result.vehicle && result.vehicle->simulated && result.vehicle->climate_on == climate_on &&
                result.vehicle->action_applied == action_applied && result.vehicle->left_front_window_open == window_open,
            "terminal result retains actual simulated state and this task action fact");
}

void fake_vehicle_set_contract() {
    FakeVehicle vehicle(false, false);
    require(!vehicle.climate_on(), "FakeVehicle initialOff is explicit");
    require(vehicle.set_climate(true) && vehicle.set_climate(true) && vehicle.climate_on(),
            "repeated climate-on sets state and never toggles it");
    require(!vehicle.set_climate(false) && !vehicle.set_climate(false) && !vehicle.climate_on(),
            "repeated climate-off sets state and returns actual state");
    FakeVehicle initial_on(true, false);
    require(initial_on.climate_on(), "constructor honours explicit initial state");
    require(!initial_on.left_front_window_open(), "window initialClosed is explicit, independent of climate");
    require(initial_on.set_left_front_window(true) && initial_on.set_left_front_window(true) && initial_on.climate_on(),
            "repeated window-open sets state and does not change AC");
    require(!initial_on.set_climate(false) && initial_on.left_front_window_open(), "climate set does not close window");
    require(!initial_on.set_left_front_window(false) && !initial_on.set_left_front_window(false), "repeated window-close never toggles");
    FakeVehicle initially_open(false, true);
    require(initially_open.left_front_window_open() && !initially_open.climate_on(), "constructor accepts explicit window initialOpen");
}

void vehicle_routing_and_failure_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    const auto first = pipeline.run_text("打开空调");
    require(first.status == VoiceStatus::kCompleted && first.answer == "模拟空调已打开。", "climate receipt comes from actual set");
    require_vehicle(first, true, true, false);
    require_vehicle(pipeline.run_text("打开空调"), true, true, false);
    require(evidence.llm_calls == 0, "explicit climate commands bypass LLM");
    evidence.asr_transcript = "关闭空调";
    const auto audio_command = pipeline.run_wav({"test-audio"});
    require(audio_command.status == VoiceStatus::kCompleted && audio_command.transcript == "关闭空调",
            "ASR root uses its transcript for the same Router vehicle path");
    require_vehicle(audio_command, false, true, false);
    require_vehicle(pipeline.run_text("打开空调"), true, true, false);
    const auto tts_calls_before = evidence.tts_calls.load();
    const auto unsupported = pipeline.run_text("打开座椅加热");
    require(unsupported.status == VoiceStatus::kFailed && unsupported.detail == "unsupported_vehicle_intent" &&
                evidence.llm_calls == 0 && evidence.tts_calls == tts_calls_before,
            "recognized unsupported vehicle intent fails without inventing execution or consulting LLM");
    require_vehicle(unsupported, true, false, false);
    evidence.llm_reply = "空调已关闭。";
    const auto words = pipeline.run_text("请描述空调状态");
    require(words.status == VoiceStatus::kCompleted && words.answer == evidence.llm_reply, "ordinary answer is preserved");
    require_vehicle(words, true, false, false);
    evidence.fail_tts = true;
    const auto failed = pipeline.run_text("关闭空调");
    require(failed.status == VoiceStatus::kFailed && failed.detail == "injected_tts_failure", "TTS failure remains explicit");
    require_vehicle(failed, false, true, false);
    const auto inherited = pipeline.run_text("一般问题");
    require(inherited.status == VoiceStatus::kFailed, "later unrelated TTS failure is separate task");
    require_vehicle(inherited, false, false, false);
    evidence.fail_tts = false;
    evidence.throw_tts = true;
    const auto thrown = pipeline.run_text("打开空调");
    require(thrown.status == VoiceStatus::kFailed && thrown.detail == "injected_tts_exception", "SDK exception becomes failed terminal");
    require_vehicle(thrown, true, true, false);
    evidence.throw_tts = false;
    evidence.oversized_audio = true;
    const auto oversized = pipeline.run_text("关闭空调");
    require(oversized.status == VoiceStatus::kFailed && oversized.detail == "output_frame_exceeds_limit", "oversized audio is explicitly rejected");
    require_vehicle(oversized, false, true, false);
    evidence.oversized_audio = false;
    evidence.fail_llm = true;
    const auto llm_failed = pipeline.run_text("另一个一般问题");
    require(llm_failed.status == VoiceStatus::kFailed, "LLM failure does not execute vehicle action");
    require_vehicle(llm_failed, false, false, false);
}

void vehicle_initial_state_failure_contract() {
    Evidence evidence;
    evidence.fail_llm = true;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(true, false));
    const auto failed = pipeline.run_text("一般问题");
    require(failed.status == VoiceStatus::kFailed, "initial-state observation accompanies LLM failure");
    require_vehicle(failed, true, false, false);
    const auto invalid = pipeline.run_text("");
    require(invalid.status == VoiceStatus::kFailed, "invalid admitted business input still ends root work");
    require_vehicle(invalid, true, false, false);
    const auto oversized = pipeline.run_wav({std::string(4 * 1024 * 1024, 'x')});
    require(oversized.status == VoiceStatus::kFailed && !oversized.vehicle,
            "preadmission frame failure does not invent a terminal vehicle observation");
}

void window_routing_and_failure_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence), std::make_unique<TestLlm>(evidence),
                           std::make_unique<TestTts>(evidence), std::make_unique<FakeVehicle>(true, false));
    for (int index = 0; index < 4; ++index) {
        const bool open = index < 2;
        const auto result = pipeline.run_text(open ? "打开左前车窗" : "关闭左前车窗");
        require(result.status == VoiceStatus::kCompleted && result.answer == (open ? "模拟左前车窗已打开。" : "模拟左前车窗已关闭。"),
                "window set produces an explicit simulation answer");
        require_vehicle(result, true, true, open);
    }
    require(evidence.llm_calls == 0, "window intents bypass LLM");
    evidence.asr_transcript = "打开左前车窗";
    const auto asr = pipeline.run_wav({"test-asr-input"});
    require(asr.status == VoiceStatus::kCompleted && asr.transcript == evidence.asr_transcript, "ASR feeds the same window Router");
    require_vehicle(asr, true, true, true);
    require_vehicle(pipeline.run_text("关闭空调"), false, true, true);
    evidence.llm_reply = "左前车窗已关闭。";
    require_vehicle(pipeline.run_text("描述车窗状态"), false, false, true);
    require_vehicle(pipeline.run_text("打开车窗"), false, false, true);  // 不猜测未明确指定的位置。
    evidence.fail_tts = true;
    const auto failed = pipeline.run_text("关闭左前车窗");
    require(failed.status == VoiceStatus::kFailed && failed.answer.empty() && failed.audio.bytes.empty(), "TTS failure publishes no stale speech");
    require_vehicle(failed, false, true, false);
    evidence.fail_tts = false; evidence.throw_tts = true;
    require_vehicle(pipeline.run_text("打开左前车窗"), false, true, true);
    evidence.throw_tts = false; evidence.oversized_audio = true;
    const auto oversized = pipeline.run_text("关闭左前车窗");
    require(oversized.status == VoiceStatus::kFailed && oversized.detail == "output_frame_exceeds_limit", "oversize still retains window fact");
    require_vehicle(oversized, false, true, false);
}

void window_cancellation_contract(bool after_action) {
    Evidence evidence; evidence.block_asr = !after_action;
    evidence.asr_transcript = "打开左前车窗"; evidence.block_tts = after_action;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence), std::make_unique<TestLlm>(evidence),
                           std::make_unique<TestTts>(evidence), std::make_unique<FakeVehicle>(true, false));
    VoiceResult result; std::atomic<bool> returned{false};
    std::thread runner([&] { result = after_action ? pipeline.run_text("打开左前车窗") : pipeline.run_wav({"test-input"}); returned = true; });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(evidence.mutex);
        entered = evidence.changed.wait_for(lock, std::chrono::seconds(2), [&] { return evidence.entered; });
    }
    const bool accepted = entered && pipeline.cancel(); const bool returned_early = returned.load();
    { std::lock_guard<std::mutex> lock(evidence.mutex); evidence.released = true; }
    evidence.changed.notify_one(); runner.join();
    require(entered && accepted && !returned_early && result.status == VoiceStatus::kCancelled, "window cancellation waits for SDK cleanup");
    require(result.answer.empty() && result.audio.bytes.empty(), "cancelled window task suppresses speech");
    require_vehicle(result, true, after_action, after_action);
    evidence.block_asr = false; evidence.block_tts = false;
    require_vehicle(pipeline.run_text("后续问答"), true, false, after_action);
}

void vehicle_cancellation_contract(bool after_action) {
    Evidence evidence;
    evidence.block_asr = !after_action;
    evidence.asr_transcript = "打开空调";
    evidence.block_tts = after_action;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence),
                           std::make_unique<TestLlm>(evidence), std::make_unique<TestTts>(evidence),
                           std::make_unique<FakeVehicle>(false, false));
    VoiceResult result;
    std::atomic<bool> returned{false};
    std::thread runner([&] {
        result = after_action ? pipeline.run_text("打开空调") : pipeline.run_wav({"test-audio"});
        returned = true;
    });
    bool entered;
    {
        std::unique_lock<std::mutex> lock(evidence.mutex);
        entered = evidence.changed.wait_for(lock, std::chrono::seconds(2), [&] { return evidence.entered; });
    }
    const bool cancel_accepted = entered && pipeline.cancel();
    const bool returned_before_release = returned.load();
    {
        std::lock_guard<std::mutex> lock(evidence.mutex);
        evidence.released = true;
    }
    evidence.changed.notify_one();
    runner.join();
    require(entered && cancel_accepted && !returned_before_release && result.status == VoiceStatus::kCancelled,
            "cancellation waits for actual SDK return and commits cancelled outcome");
    require(result.audio.bytes.empty() && result.answer.empty(), "cancelled speech/audio is suppressed");
    require_vehicle(result, after_action, after_action, false);
    require(evidence.llm_calls == 0 && evidence.tts_calls == (after_action ? 1 : 0),
            "pre-action cancel invokes no action TTS; post-action cancel runs no extra handler");
    evidence.block_asr = false;
    evidence.block_tts = false;
    const auto next = pipeline.run_text("取消清理后的一般问题");
    require(next.status == VoiceStatus::kCompleted, "new work runs after cancelled work cleanup");
    require_vehicle(next, after_action, false, false);
}
}

void music_routing_contract() {
    Evidence evidence;
    VoicePipeline pipeline(std::make_unique<TestAsr>(evidence), std::make_unique<TestLlm>(evidence),
                           std::make_unique<TestTts>(evidence), std::make_unique<FakeVehicle>(false, false));
    using Command = cabinflow::agent::v1::MusicCommand;
    const std::pair<const char*, Command::Action> commands[] = {
        {"搜索周杰伦", Command::SEARCH}, {"播放音乐", Command::PLAY}, {"暂停音乐", Command::PAUSE},
        {"上一首", Command::PREVIOUS}, {"下一首", Command::NEXT}, {"播放第一首", Command::SELECT},
        {"播放第二首", Command::SELECT}, {"播放第11首", Command::SELECT}};
    for (const auto& [text, action] : commands) {
        const auto result = pipeline.run_text(text);
        require(result.status == VoiceStatus::kCompleted && result.music_command && result.music_command->action() == action &&
            result.audio.bytes.empty() && result.answer.empty(), "music produces typed instruction, not speech or playback receipt");
        require(result.vehicle && !result.vehicle->action_applied && !result.vehicle->climate_on &&
            !result.vehicle->left_front_window_open, "music cannot mutate fake vehicle state");
    }
    const auto search = pipeline.run_text("搜索  周杰伦  ");
    require(search.music_command && search.music_command->keyword() == "周杰伦", "search keyword explicitly trimmed");
    for (const auto* invalid : {"搜索", "播放第0首", "播放第abc首"})
        require(pipeline.run_text(invalid).status == VoiceStatus::kFailed, "recognized invalid music format fails explicitly, not LLM fallback");
    evidence.asr_transcript = "暂停音乐";
    const auto audio = pipeline.run_wav({"test-input"});
    require(audio.music_command && audio.music_command->action() == Command::PAUSE && evidence.asr_calls == 1 &&
        evidence.llm_calls == 0 && evidence.tts_calls == 0, "ASR and text share one Router; music bypasses LLM/TTS");
}

int main() {
    try {
        text_and_failure_contract();
        cancellation_contract(false);
        cancellation_contract(true);
        frame_boundary_contract();
        candidate_commit_contract();
        root_not_handled_contract();
        invalid_business_and_rollback_contract();
        concurrent_exit_and_completion_contract();
        gate_exception_cleanup_contract();
        shutdown_waits_for_downstream_contract();
        fake_vehicle_set_contract();
        vehicle_routing_and_failure_contract();
        vehicle_initial_state_failure_contract();
        vehicle_cancellation_contract(false);
        vehicle_cancellation_contract(true);
        window_routing_and_failure_contract();
        window_cancellation_contract(false);
        window_cancellation_contract(true);
        music_routing_contract();
        std::cout << "voice pipeline contracts passed; test backends are not real inference\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
