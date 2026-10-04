#include <cabinflow/agent/voice_pipeline.hpp>
#include <cabinflow/agent/fake_vehicle.hpp>
#include <cabinflow/gateway/runtime_message_framer.hpp>
#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cockpit_audio.pb.h>
#include <cockpit_task.pb.h>
#include <cockpit_text.pb.h>
#include <control.pb.h>
#include <delivery.pb.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
using namespace cabinflow::agent::inference;
using namespace std::chrono_literals;
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }

// 测试替身只固定端口与生命周期行为，不是生产模型或可听语音。
struct Evidence {
    std::mutex mutex;
    std::condition_variable changed;
    bool block{false}, entered{false}, release{false};
    bool fail_tts{false};
    std::atomic<unsigned> llm_calls{0};
    std::atomic<unsigned> tts_calls{0};
};
class Asr final : public AsrBackend {
    BackendResult<std::string> transcribe(std::string_view, const CancellationCheck&) override {
        return {"识别的问题", BackendError::kNone, {}};
    }
};
class Llm final : public LlmBackend {
public:
    explicit Llm(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<std::string> generate(std::string_view input, const CancellationCheck&) override {
        ++evidence_.llm_calls;
        if (input == "虚构车控") return {"已打开空调。", BackendError::kNone, {}};
        return {"answer:" + std::string(input), BackendError::kNone, {}};
    }
private:
    Evidence& evidence_;
};
class Tts final : public TtsBackend {
public:
    explicit Tts(Evidence& evidence) : evidence_(evidence) {}
    BackendResult<WavAudio> synthesize(std::string_view, const CancellationCheck&) override {
        ++evidence_.tts_calls;
        std::unique_lock<std::mutex> lock(evidence_.mutex);
        if (evidence_.block) {
            evidence_.entered = true;
            evidence_.changed.notify_all();
            evidence_.changed.wait(lock, [this] { return evidence_.release; });
        }
        if (evidence_.fail_tts) return {{}, BackendError::kInferenceFailed, "test TTS failure"};
        return {{"test-audio"}, BackendError::kNone, {}};
    }
private:
    Evidence& evidence_;
};
class Fixture {
public:
    Fixture() : pipeline(std::make_unique<Asr>(), std::make_unique<Llm>(evidence), std::make_unique<Tts>(evidence),
                         std::make_unique<cabinflow::agent::FakeVehicle>(false, false)) {
        auto ready = std::make_shared<std::promise<void>>();
        auto result = ready->get_future();
        thread_ = std::thread([this, ready] {
            cabinflow::net::EventLoop loop;
            loop_ = &loop;
            gateway_ = std::make_unique<cabinflow::gateway::ControlGateway>(loop, "voice-test", "127.0.0.1", 0,
                pipeline.registry(), pipeline.runtime(), pipeline.clock(), 300'000);
            gateway_->set_data_task_hooks(pipeline.hooks());
            gateway_->start();
            port_ = gateway_->bound_port();
            ready->set_value();
            loop.loop();
            gateway_->stop();
            gateway_.reset();
        });
        result.get();
    }
    ~Fixture() {
        // 断言失败也先释放 SDK 屏障；Gateway 必须活到根 completion/drain 之后。
        release();
        pipeline.shutdown();
        loop_->quit();
        thread_.join();
    }
    void release() {
        { std::lock_guard<std::mutex> lock(evidence.mutex); evidence.release = true; }
        evidence.changed.notify_all();
    }
    std::uint16_t port() const { return port_; }
    Evidence evidence;
    cabinflow::agent::VoicePipeline pipeline;
private:
    cabinflow::net::EventLoop* loop_{nullptr};
    std::unique_ptr<cabinflow::gateway::ControlGateway> gateway_;
    std::uint16_t port_{0};
    std::thread thread_;
};
class Client {
public:
    explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)) {
        require(fd_ >= 0, "create TCP socket");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect TCP gateway");
    }
    ~Client() { ::close(fd_); }
    void send(const Message& message) {
        const auto frame = cabinflow::gateway::RuntimeMessageFramer::encode(message);
        require(static_cast<bool>(frame), "encode request frame");
        for (std::size_t sent = 0; sent < frame.bytes.size();) {
            const auto count = ::send(fd_, frame.bytes.data() + sent, frame.bytes.size() - sent, MSG_NOSIGNAL);
            require(count > 0, "send request"); sent += static_cast<std::size_t>(count);
        }
    }
    Message read() {
        const auto header = exact(4);
        std::uint32_t length = 0;
        for (unsigned char byte : header) length = (length << 8U) | byte;
        require(length > 0 && length <= cabinflow::gateway::kMaxFrameBytes, "response length within frame limit");
        cabinflow::gateway::RuntimeMessageFramer framer;
        auto result = framer.feed(header + exact(length));
        require(result && result.messages.size() == 1, "decode one response");
        return result.messages.front();
    }
    void no_response() {
        pollfd p{fd_, POLLIN, 0}; require(::poll(&p, 1, 50) == 0, "no ACK or premature task result");
    }
private:
    std::string exact(std::size_t length) {
        std::string bytes(length, '\0');
        for (std::size_t used = 0; used < length;) {
            pollfd p{fd_, POLLIN, 0}; require(::poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "response timeout");
            const auto count = ::recv(fd_, bytes.data() + used, length - used, 0);
            require(count > 0, "connection closed before response"); used += static_cast<std::size_t>(count);
        }
        return bytes;
    }
    int fd_;
};
Message envelope(std::string id, std::string session, std::string work, std::string target, std::string topic) {
    Message message;
    auto& e = message.envelope;
    e.message_id = std::move(id); e.trace_id = "trace:" + e.message_id;
    e.session_id = std::move(session); e.work_id = std::move(work);
    e.source_node = "tcp.client"; e.target_node = std::move(target); e.topic = std::move(topic);
    e.created_monotonic_ns = cabinflow::runtime::SteadyClock{}.now_monotonic_ns();
    e.ttl_ms = 300'000; e.is_final = true;
    return message;
}
std::string setup(Client& client, std::string id, const std::string& session, const char* unit = "dialogue.primary") {
    auto request = envelope(std::move(id), session, {}, "runtime.control", "control.request");
    cabinflow::protocol::v1::ControlRequest body; body.mutable_setup()->set_unit_id(unit);
    request.payload = body.SerializeAsString(); client.send(request);
    const auto message = client.read();
    cabinflow::protocol::v1::ControlResponse response;
    require(response.ParseFromString(message.payload) && response.has_setup(), "setup creates work");
    require(response.request_message_id() == request.envelope.message_id && message.envelope.message_id != request.envelope.message_id,
            "control request association distinct from response identity");
    return response.setup().work().work_id();
}
Message text(const std::string& id, const std::string& session, const std::string& work, const char* value = "测试问题") {
    auto request = envelope(id, session, work, "dialogue.primary", "cockpit.text.input");
    cabinflow::agent::v1::TextInput body; body.set_text(value); request.payload = body.SerializeAsString();
    return request;
}
cabinflow::agent::v1::VoiceTaskResult terminal(Client& client, const Message& input, bool success) {
    const auto message = client.read();
    const auto& e = message.envelope;
    require(e.topic == "cockpit.task.result" && e.is_final && e.kind == (success ? MessageKind::kData : MessageKind::kError),
            "one typed final task outcome, not intermediate output");
    require(e.message_id != input.envelope.message_id && e.session_id == input.envelope.session_id &&
        e.work_id == input.envelope.work_id && e.trace_id == input.envelope.trace_id &&
        e.source_node == input.envelope.target_node && e.target_node == input.envelope.source_node,
        "task result returns to original request identity and connection");
    cabinflow::agent::v1::VoiceTaskResult result;
    require(result.ParseFromString(message.payload) && result.request_message_id() == input.envelope.message_id,
            "typed result explicit request association");
    require(result.has_vehicle() && result.vehicle().simulated() && result.vehicle().has_left_front_window_open(),
            "every task outcome carries complete simulated vehicle facts");
    return result;
}
void exit(Client& client, const std::string& id, const std::string& session, const std::string& work, const char* reason = "test cancellation") {
    auto request = envelope(id, session, work, "runtime.control", "control.request");
    cabinflow::protocol::v1::ControlRequest body; body.mutable_exit()->set_reason(reason);
    request.payload = body.SerializeAsString(); client.send(request);
    cabinflow::protocol::v1::ControlResponse response;
    require(response.ParseFromString(client.read().payload), "decode ExitResponse");
    require(reason[0] ? response.has_exit() : response.has_error(), "exit accepts only explicit reason");
}
void success_and_invalid() {
    Fixture fixture; Client client(fixture.port());
    const auto work = setup(client, "setup-one", "session-one");
    auto input = text("input-one", "session-one", work); client.send(input);
    auto result = terminal(client, input, true);
    require(result.has_audio() && result.audio().request_message_id() == input.envelope.message_id &&
        result.answer() == "answer:测试问题" && !result.audio().wav_bytes().empty(), "all stages reach correlated audio");
    client.no_response();
    require(fixture.pipeline.registry().find_work("session-one", work).work.state == cabinflow::runtime::WorkState::kExited,
            "terminal comes after work cleanup");
    for (bool final : {true, false}) {
        const auto session = final ? "invalid-payload" : "invalid-final";
        const auto bad_work = setup(client, session, session);
        auto bad = text(std::string(session) + ":input", session, bad_work);
        bad.envelope.is_final = final; if (final) bad.payload = "\xff";
        client.send(bad); const auto failed = terminal(client, bad, false);
        require(failed.has_failed() && failed.answer().empty() && !failed.has_audio(), "invalid business input terminates work");
        require(fixture.pipeline.registry().find_work(session, bad_work).work.state == cabinflow::runtime::WorkState::kExited,
                "invalid work cleaned, no ledger rollback path");
        client.no_response();
    }
    auto next = text("next-input", "next-session", setup(client, "next-setup", "next-session"));
    client.send(next); require(terminal(client, next, true).has_audio(), "fresh work succeeds after invalid input");
}
void cancellation_and_busy() {
    Fixture fixture; { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.block = true; }
    Client first(fixture.port()), second(fixture.port());
    const auto work = setup(first, "setup-blocked", "blocked");
    auto input = text("blocked-input", "blocked", work); first.send(input);
    {
        std::unique_lock<std::mutex> lock(fixture.evidence.mutex);
        require(fixture.evidence.changed.wait_for(lock, 2s, [&] { return fixture.evidence.entered; }), "TTS actually running");
    }
    exit(first, "invalid-exit", "blocked", work, "");
    first.no_response();
    exit(first, "valid-exit", "blocked", work);
    first.no_response();
    const auto busy_work = setup(second, "setup-busy", "busy", "asr.primary");
    auto busy = envelope("busy-input", "busy", busy_work, "asr.primary", "cockpit.audio.input");
    cabinflow::agent::v1::AudioInput audio; audio.set_wav_bytes("test-input"); busy.payload = audio.SerializeAsString(); second.send(busy);
    const auto busy_message = second.read(); cabinflow::protocol::v1::DeliveryError rejected;
    require(busy_message.envelope.topic == "runtime.delivery.error" && rejected.ParseFromString(busy_message.payload) &&
        rejected.code() == cabinflow::protocol::v1::DELIVERY_ERROR_QUEUE_FULL, "global single-task admission stays busy during SDK cleanup");
    exit(second, "cleanup-busy", "busy", busy_work);
    fixture.release();
    const auto cancelled = terminal(first, input, false);
    require(cancelled.has_cancelled() && cancelled.answer().empty() && !cancelled.has_audio(), "late SDK output suppressed after cancellation");
    first.no_response(); second.no_response();
    auto next = text("after-cancel-input", "after-cancel", setup(first, "after-cancel-setup", "after-cancel"));
    first.send(next); require(terminal(first, next, true).has_audio(), "next work succeeds only after actual cancellation cleanup");
    second.no_response();
}
void invalid_exit_preserves_task() {
    Fixture fixture; { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.block = true; }
    Client client(fixture.port());
    auto input = text("invalid-exit-input", "invalid-exit-session", setup(client, "invalid-exit-setup", "invalid-exit-session"));
    client.send(input);
    {
        std::unique_lock<std::mutex> lock(fixture.evidence.mutex);
        require(fixture.evidence.changed.wait_for(lock, 2s, [&] { return fixture.evidence.entered; }), "SDK running before invalid Exit");
    }
    exit(client, "empty-reason", "invalid-exit-session", input.envelope.work_id, "");
    fixture.release();
    require(terminal(client, input, true).has_audio(), "invalid Exit never requests application cancellation");
}
void vehicle_receipts() {
    Fixture fixture; Client client(fixture.port());
    auto invented = text("invented-input", "invented", setup(client, "invented-setup", "invented"), "虚构车控");
    client.send(invented);
    auto result = terminal(client, invented, true);
    require(result.answer() == "已打开空调。" && !result.vehicle().climate_on() && !result.vehicle().action_applied(),
            "LLM claims cannot execute vehicle actions");
    for (unsigned index = 0; index < 4; ++index) {
        const bool on = index < 2;
        const auto session = "climate-" + std::to_string(index);
        auto input = text(session + ":input", session, setup(client, session + ":setup", session), on ? "打开空调" : "关闭空调");
        client.send(input); result = terminal(client, input, true);
        require(result.has_audio() && result.vehicle().climate_on() == on && result.vehicle().action_applied() &&
                result.answer().find("模拟") != std::string::npos, "ON/ON/OFF/OFF receipts are set operations, not toggles");
        client.no_response();
    }
    require(fixture.evidence.llm_calls == 1, "explicit climate actions do not enter LLM");
    { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.fail_tts = true; }
    auto failed = text("failed-climate-input", "failed-climate", setup(client, "failed-climate-setup", "failed-climate"), "打开空调");
    client.send(failed); result = terminal(client, failed, false);
    require(result.has_failed() && result.vehicle().climate_on() && result.vehicle().action_applied() &&
            result.answer().empty() && !result.has_audio(), "TTS failure cannot erase committed vehicle fact");

    auto invalid = text("invalid-after-action", "invalid-after-action", setup(client, "invalid-after-action-setup", "invalid-after-action"));
    invalid.payload = "\xff"; client.send(invalid); result = terminal(client, invalid, false);
    require(result.has_failed() && result.vehicle().climate_on() && !result.vehicle().action_applied(),
            "invalid task reports existing state without claiming another action");

    { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.fail_tts = false; fixture.evidence.block = true; }
    auto cancelled = text("cancel-climate-input", "cancel-climate", setup(client, "cancel-climate-setup", "cancel-climate"), "关闭空调");
    client.send(cancelled);
    {
        std::unique_lock<std::mutex> lock(fixture.evidence.mutex);
        require(fixture.evidence.changed.wait_for(lock, 2s, [&] { return fixture.evidence.entered; }), "action completed before TTS barrier");
    }
    exit(client, "cancel-climate-exit", "cancel-climate", cancelled.envelope.work_id);
    client.no_response(); fixture.release(); result = terminal(client, cancelled, false);
    require(result.has_cancelled() && !result.vehicle().climate_on() && result.vehicle().action_applied() &&
            result.answer().empty() && !result.has_audio(), "cancel after action preserves OFF receipt, suppresses late speech");
    auto next = text("state-next-input", "state-next", setup(client, "state-next-setup", "state-next"));
    client.send(next); result = terminal(client, next, true);
    require(!result.vehicle().climate_on() && !result.vehicle().action_applied(), "vehicle state persists across fresh works");
}
void window_receipts() {
    Fixture fixture; Client client(fixture.port());
    auto climate = text("window-ac-input", "window-ac", setup(client, "window-ac-setup", "window-ac"), "打开空调");
    client.send(climate); require(terminal(client, climate, true).vehicle().climate_on(), "AC on before window commands");
    for (unsigned index = 0; index < 4; ++index) {
        const bool open = index < 2; const auto session = "window-" + std::to_string(index);
        auto input = text(session + ":input", session, setup(client, session + ":setup", session), open ? "打开左前车窗" : "关闭左前车窗");
        client.send(input); const auto result = terminal(client, input, true);
        require(result.has_audio() && result.vehicle().left_front_window_open() == open && result.vehicle().climate_on() &&
                result.vehicle().action_applied(), "TCP window OPEN/OPEN/CLOSE/CLOSE receipts preserve AC");
        client.no_response();
    }
    require(fixture.evidence.llm_calls == 0, "TCP window controls use Router, not LLM");
    { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.fail_tts = true; }
    auto failed = text("window-failed-input", "window-failed", setup(client, "window-failed-setup", "window-failed"), "打开左前车窗");
    client.send(failed); const auto failure = terminal(client, failed, false);
    require(failure.has_failed() && failure.vehicle().left_front_window_open() && failure.vehicle().climate_on() &&
            failure.vehicle().action_applied() && !failure.has_audio(), "TCP TTS failure cannot roll back window state");
    { std::lock_guard<std::mutex> lock(fixture.evidence.mutex); fixture.evidence.fail_tts = false; fixture.evidence.block = true; }
    auto cancelled = text("window-cancel-input", "window-cancel", setup(client, "window-cancel-setup", "window-cancel"), "关闭左前车窗");
    client.send(cancelled);
    {
        std::unique_lock<std::mutex> lock(fixture.evidence.mutex);
        require(fixture.evidence.changed.wait_for(lock, 2s, [&] { return fixture.evidence.entered; }), "window action reaches TTS barrier");
    }
    exit(client, "window-cancel-exit", "window-cancel", cancelled.envelope.work_id);
    fixture.release(); const auto result = terminal(client, cancelled, false);
    require(result.has_cancelled() && !result.vehicle().left_front_window_open() && result.vehicle().climate_on() &&
            result.vehicle().action_applied() && !result.has_audio(), "TCP cancel keeps window action fact, suppresses audio");
}
void music_commands() {
    Fixture fixture; Client client(fixture.port());
    using Command = cabinflow::agent::v1::MusicCommand;
    for (const auto& item : {std::pair<const char*, Command::Action>{"搜索周杰伦", Command::SEARCH},
                            {"播放第二首", Command::SELECT}, {"暂停音乐", Command::PAUSE}}) {
        const auto session = "music-" + std::to_string(static_cast<int>(item.second));
        auto input = text(session + ":input", session, setup(client, session + ":setup", session), item.first);
        client.send(input); const auto result = terminal(client, input, true);
        require(result.has_music_command() && result.music_command().action() == item.second &&
            !result.has_audio() && !result.vehicle().action_applied(), "real TCP transports typed non-audio instruction, not playback success");
        if (item.second == Command::SELECT) require(result.music_command().result_index() == 2, "one-based voice selection preserved");
        client.no_response();
    }
    require(fixture.evidence.llm_calls == 0 && fixture.evidence.tts_calls == 0, "music skips both model generation and synthesis");
}
}  // namespace
int main() {
    try { success_and_invalid(); cancellation_and_busy(); invalid_exit_preserves_task(); vehicle_receipts(); window_receipts(); music_commands(); std::cout << "voice_gateway_tcp_test passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
