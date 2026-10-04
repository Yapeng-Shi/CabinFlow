#include "voice_client.hpp"

#include <chrono>
#include <utility>

#include <QDir>
#include <QFile>
#include <QUuid>

#include <cockpit_audio.pb.h>
#include <cockpit_task.pb.h>
#include <cockpit_text.pb.h>
#include <control.pb.h>
#include <delivery.pb.h>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/agent/music_command.hpp>

namespace {
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
std::string unique_id() { return QUuid::createUuid().toString(QUuid::Id128).toStdString(); }
}

VoiceClient::VoiceClient(QString host, quint16 port, MusicController& music, QObject* parent)
    : QObject(parent), host_(std::move(host)), port_(port), music_(music) {
    player_.setAudioOutput(&audio_output_);
    connect(&socket_, &QTcpSocket::connected, this, [this] { status_ = "就绪"; emit changed(); });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        clearAudio();
        controls_.clear();
        busy_ = false;
        music_.releaseVoice();
        vehicle_known_ = false;
        vehicle_action_ = "断连，执行状态未知";
        status_ = "已断连";
        error_ = "连接已断开；后端任务状态未知，不自动重连";
        emit changed();
        maybeClose();
    });
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](auto) { fail(socket_.errorString()); });
    connect(&socket_, &QTcpSocket::readyRead, this, [this] {
        const auto bytes = socket_.readAll();
        const auto framed = framer_.feed(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        for (const auto& message : framed.messages) receive(message);
        if (!framed) fail("非法 TCP 帧；不尝试其他协议", true);
    });
    connect(&player_, &QMediaPlayer::playbackStateChanged, this, [this](auto state) {
        if (clearing_audio_) return;
        if (state == QMediaPlayer::StoppedState) {
            ++answer_generation_; answer_pending_ = false; music_.stopAnswer();
        }
        emit changed();
    });
    connect(&player_, &QMediaPlayer::errorOccurred, this, [this](auto, const QString& detail) {
        error_ = "播放失败：" + detail;
        ++answer_generation_; answer_pending_ = false; music_.stopAnswer();
        emit changed();
    });
    connect(&music_, &MusicController::answerReady, this, [this](quint64 generation) {
        if (generation != answer_generation_ || !answer_pending_ || busy_ || closing_ || !audio_file_) return;
        answer_pending_ = false; player_.play();
    });
    connect(&music_, &MusicController::answerFailed, this, [this](quint64 generation, const QString& detail) {
        if (generation != answer_generation_ || !answer_pending_) return;
        answer_pending_ = false; error_ = detail; emit changed();
    });
    connect(&music_, &MusicController::closeReady, this, &VoiceClient::maybeClose);
}

VoiceClient::~VoiceClient() {
    // 成员析构仍可能触发信号；先断开回调，再停播放器/解除文件，避免访问已销毁的状态。
    socket_.disconnect(this);
    player_.disconnect(this);
    music_.disconnect(this);
    clearAudio();
    socket_.abort();
}

void VoiceClient::connectBackend() {
    if (busy_ || socket_.state() != QAbstractSocket::UnconnectedState) return;
    framer_ = cabinflow::gateway::RuntimeMessageFramer{};
    controls_.clear();
    vehicle_known_ = false;
    vehicle_action_ = "尚未收到模拟执行回执";
    status_ = "连接中";
    error_.clear();
    emit changed();
    // 所有 socket 操作靠 Qt 事件驱动；GUI 不 waitForConnected/readyRead 或执行模型。
    socket_.connectToHost(host_, port_);
}

Message VoiceClient::envelope(std::string topic, std::string target) {
    Message message;
    auto& e = message.envelope;
    e.message_id = unique_id();
    e.trace_id = trace_;
    e.session_id = session_;
    e.work_id = work_;
    e.source_node = "qt.client";
    e.target_node = std::move(target);
    e.topic = std::move(topic);
    e.created_monotonic_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    e.ttl_ms = 300'000;
    e.is_final = true;
    return message;
}

bool VoiceClient::send(const Message& message) {
    const auto frame = cabinflow::gateway::RuntimeMessageFramer::encode(message);
    if (!frame || !connected()) { fail("消息超限、无效或连接未建立"); return false; }
    const auto bytes = socket_.write(frame.bytes.data(), static_cast<qint64>(frame.bytes.size()));
    if (bytes != static_cast<qint64>(frame.bytes.size())) { fail("TCP 发送入队失败", true); return false; }
    return true;
}

void VoiceClient::startText(const QString& text) {
    if (text.isEmpty() || text.toUtf8().size() > 16 * 1024) { error_ = "请输入不超过 16 KiB 的文本"; emit changed(); return; }
    cabinflow::agent::v1::TextInput input;
    input.set_text(text.toStdString());
    begin(false, input.SerializeAsString());
}

void VoiceClient::startWav(const QUrl& file) {
    if (!file.isLocalFile()) { error_ = "请选择本地 WAV"; emit changed(); return; }
    QFile wav(file.toLocalFile());
    if (!wav.open(QIODevice::ReadOnly) || wav.size() <= 0 || wav.size() > 4 * 1024 * 1024) {
        error_ = "WAV 不可读取、为空或超限"; emit changed(); return;
    }
    const auto bytes = wav.readAll();
    if (wav.error() != QFileDevice::NoError || bytes.size() != wav.size()) {
        error_ = "WAV 读取失败"; emit changed(); return;
    }
    cabinflow::agent::v1::AudioInput input;
    input.set_wav_bytes(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    begin(true, input.SerializeAsString());
}

void VoiceClient::begin(bool audio, std::string payload) {
    if (busy_ || closing_ || !connected()) return;
    if (music_.busy()) { error_ = "音乐操作尚未静止，请稍后开始语音任务"; emit changed(); return; }
    clearAudio();
    if (!music_.acquireVoice()) { error_ = "等待音乐音量恢复后再开始任务"; emit changed(); return; }
    transcript_.clear(); answer_.clear(); error_.clear();
    session_ = unique_id(); trace_ = unique_id(); work_.clear(); input_id_.clear();
    audio_input_ = audio; payload_ = std::move(payload);
    unit_ = audio ? "asr.primary" : "dialogue.primary";
    cancel_requested_ = false; input_sent_ = false; cleanup_rejection_ = false;
    terminal_consumed_ = false;
    busy_ = true; status_ = "创建任务";
    vehicle_action_ = "本次动作：等待回执";
    auto request = envelope("control.request", "runtime.control");
    cabinflow::protocol::v1::ControlRequest setup;
    setup.mutable_setup()->set_unit_id(unit_);
    request.payload = setup.SerializeAsString();
    controls_.emplace(request.envelope.message_id, ControlPending{true, false, session_, {}, trace_, unit_});
    static_cast<void>(send(request));
    emit changed();
}

void VoiceClient::sendExit(bool cleanup_only) {
    if (work_.empty()) return;
    for (auto& item : controls_) {
        if (!item.second.setup && item.second.session == session_ && item.second.work == work_) {
            // DeliveryError 证明没有准入：已发出的 Exit 升级为纯清理，不等不存在的业务终态。
            item.second.cleanup_only = item.second.cleanup_only || cleanup_only;
            return;
        }
    }
    auto request = envelope("control.request", "runtime.control");
    cabinflow::protocol::v1::ControlRequest exit;
    exit.mutable_exit()->set_reason(cleanup_only ? "frontend cleanup before admission" : "frontend user cancellation");
    request.payload = exit.SerializeAsString();
    controls_.emplace(request.envelope.message_id, ControlPending{false, cleanup_only, session_, work_, trace_, unit_});
    static_cast<void>(send(request));
}

void VoiceClient::receive(const Message& message) {
    const auto& e = message.envelope;
    if (e.schema_version != cabinflow::protocol::kCurrentSchemaVersion || !e.is_final ||
        e.target_node != "qt.client" || e.message_id.empty()) {
        fail("响应 Envelope 无效", true); return;
    }
    if (e.topic == "control.response") {
        cabinflow::protocol::v1::ControlResponse response;
        if (!response.ParseFromString(message.payload)) { fail("控制响应损坏", true); return; }
        const auto found = controls_.find(response.request_message_id());
        if (found == controls_.end()) { fail("未知控制请求关联", true); return; }
        const auto pending = found->second;
        controls_.erase(found);
        if (e.session_id != pending.session || e.trace_id != pending.trace || e.source_node != "runtime.control" ||
            e.message_id == response.request_message_id() ||
            e.kind != (response.has_error() ? MessageKind::kError : MessageKind::kData)) {
            fail("控制响应身份/类型不一致", true); return;
        }
        if (!pending.setup && (e.work_id != pending.work || (response.has_exit() &&
            (response.exit().work().work_id() != pending.work || response.exit().work().unit_id() != pending.unit ||
             response.exit().work().state() != cabinflow::protocol::v1::WORK_STATE_EXITED)))) {
            fail("Exit 内外 work 身份/退出状态不一致", true); return;
        }
        // 新任务可能已开始；旧 ExitResponse 只完成自己的关联，不改变当前状态。
        if (pending.session != session_ || !busy_) return;
        if (pending.setup) {
            if (response.has_error()) { fail(QString::fromStdString(response.error().message())); return; }
            if (!response.has_setup() || response.setup().work().work_id().empty() ||
                e.work_id != response.setup().work().work_id() || response.setup().work().unit_id() != unit_) {
                fail("Setup 身份不一致", true); return;
            }
            work_ = e.work_id;
            // Setup 中的取消没有数据 handler；只等控制清理，不能等不存在的 TaskResult。
            if (cancel_requested_ || closing_) { sendExit(true); return; }
            auto input = envelope(audio_input_ ? "cockpit.audio.input" : "cockpit.text.input", unit_);
            input.payload = std::move(payload_);
            input_id_ = input.envelope.message_id;
            const auto frame = cabinflow::gateway::RuntimeMessageFramer::encode(input);
            if (!frame) { error_ = "完整输入编码帧超限"; cleanup_rejection_ = true; sendExit(true); return; }
            input_sent_ = true; status_ = "处理中";
            static_cast<void>(send(input));
        } else {
            if (pending.cleanup_only) {
                const bool already_absent = response.has_error() &&
                    (response.error().code() == static_cast<std::uint32_t>(cabinflow::gateway::ControlErrorCode::kWorkNotFound) ||
                     response.error().code() == static_cast<std::uint32_t>(cabinflow::gateway::ControlErrorCode::kInvalidWorkState));
                if (!response.has_exit() && !already_absent) {
                    fail(response.has_error() ? QString::fromStdString(response.error().message()) : "非法清理响应", true);
                    return;
                }
                vehicle_action_ = "本次动作：未准入";
                terminal(cleanup_rejection_ ? "失败" : "已取消");
            } else if (!response.has_exit() && !response.has_error()) {
                fail("非法 Exit 响应", true); return;
            } else if (response.has_error() && response.error().code() !=
                       static_cast<std::uint32_t>(cabinflow::gateway::ControlErrorCode::kInvalidWorkState)) {
                // 不把取消错误伪装为已清理；连接退出后状态明确未知。
                fail(QString::fromStdString(response.error().message()), true); return;
            }
            // 已准入时 Exit 成功/已退出都不是清理证明，继续等待唯一业务终态。
        }
    } else if (e.topic == "runtime.delivery.error") {
        cabinflow::protocol::v1::DeliveryError error;
        if (!error.ParseFromString(message.payload) || error.request_message_id() != input_id_ ||
            e.session_id != session_ || e.work_id != work_ || e.trace_id != trace_ || e.kind != MessageKind::kError) {
            fail("投递错误关联无效", true); return;
        }
        // 当前后端把已准入错误统一为 TaskResult；此处只处理尚未准入的 work 清理。
        error_ = QString::fromStdString(error.message()); cleanup_rejection_ = true;
        status_ = "失败，清理中"; sendExit(true);
    } else if (e.topic == "cockpit.task.result") {
        if (e.session_id != session_ || e.work_id != work_ || !busy_ || terminal_consumed_) return;
        cabinflow::agent::v1::VoiceTaskResult result;
        if (!result.ParseFromString(message.payload) || result.request_message_id() != input_id_ ||
            e.trace_id != trace_ || e.source_node != unit_ || e.message_id == input_id_ ||
            result.result_case() == cabinflow::agent::v1::VoiceTaskResult::RESULT_NOT_SET ||
            !result.has_vehicle() || !result.vehicle().simulated() ||
            !result.vehicle().has_left_front_window_open() ||
            e.kind != ((result.has_audio() || result.has_music_command()) ? MessageKind::kData : MessageKind::kError)) {
            fail("任务终态关联/类型无效", true); return;
        }
        // 完整业务身份先校验；非法音频不能在 fail 的 changed 信号中短暂暴露伪造车控状态。
        if (result.has_audio() && (result.audio().request_message_id() != input_id_ || result.audio().wav_bytes().empty())) {
            fail("回答音频关联无效", true); return;
        }
        if (result.has_music_command() && !cabinflow::agent::is_valid_music_command(result.music_command())) {
            fail("音乐指令类型或参数无效", true); return;
        }
        // 任何外部信号/播放器调用前一次性消费当前终态，重复响应不能重放音乐动作。
        terminal_consumed_ = true;
        transcript_ = QString::fromStdString(result.transcript());
        // 执行事实先于音频处理：取消、TTS 失败或本地播放器错误都不抹掉已执行的动作。
        vehicle_known_ = true;
        climate_on_ = result.vehicle().climate_on();
        left_front_window_open_ = result.vehicle().left_front_window_open();
        vehicle_action_ = result.vehicle().action_applied() ? "本次动作：已执行（模拟）" : "本次动作：未执行";
        if (result.has_music_command()) {
            answer_.clear(); clearAudio();
            if (!cancel_requested_ && !closing_) music_.executeFromVoice(result.music_command());
            terminal(cancel_requested_ || closing_ ? "已取消（音乐未发起）" : "音乐指令已识别（执行结果见中控）");
        } else if (result.has_audio()) {
            answer_ = QString::fromStdString(result.answer());
            audio_file_ = std::make_unique<QTemporaryFile>(QDir::tempPath() + "/cabinflow-XXXXXX.wav");
            if (!audio_file_->open() || audio_file_->write(result.audio().wav_bytes().data(),
                static_cast<qint64>(result.audio().wav_bytes().size())) != static_cast<qint64>(result.audio().wav_bytes().size()) ||
                !audio_file_->flush()) { fail("回答 WAV 写入失败"); return; }
            audio_file_->close();
            player_.setSource(QUrl::fromLocalFile(audio_file_->fileName()));
            terminal(cancel_requested_ ? "已完成（取消晚于结果提交）" : "已完成");
        } else if (result.has_cancelled()) {
            answer_.clear(); clearAudio();
            terminal("已取消");
        } else {
            answer_.clear(); clearAudio(); error_ = QString::fromStdString(result.failed().message());
            terminal("失败");
        }
    } else { fail("未知响应 topic", true); return; }
    emit changed();
}

void VoiceClient::cancel() {
    const bool cancelling_task = busy_;
    const auto cancelling_session = session_;
    // stopPlayback 会同步发信号，监听器可能重入并收到终态；先提交取消，禁止该终态发起音乐。
    if (cancelling_task) { cancel_requested_ = true; status_ = "取消中"; }
    stopPlayback();
    // 重入终态还可能启动下一任务，不能把旧取消发到新的 work。
    if (!cancelling_task || !busy_ || session_ != cancelling_session) return;
    if (!work_.empty()) sendExit(!input_sent_);
    emit changed();
}
void VoiceClient::play() {
    if (!busy_ && audio_file_ && !closing_ && !playing() && !answer_pending_) {
        answer_pending_ = true;
        music_.prepareAnswer(++answer_generation_);
    }
}
void VoiceClient::stopPlayback() {
    ++answer_generation_; answer_pending_ = false;
    music_.stopAnswer(); player_.stop(); emit changed();
}
void VoiceClient::clearAudio() {
    clearing_audio_ = true;
    ++answer_generation_; answer_pending_ = false; music_.stopAnswer();
    player_.stop(); player_.setSource(QUrl{}); audio_file_.reset();
    clearing_audio_ = false;
}
void VoiceClient::terminal(QString state) {
    music_.releaseVoice();
    busy_ = false; status_ = std::move(state); emit changed();
    if (closing_) clearAudio();
    maybeClose();
}
void VoiceClient::fail(QString detail, bool disconnect) {
    clearAudio(); answer_.clear(); error_ = std::move(detail); busy_ = false; status_ = "失败";
    music_.releaseVoice();
    emit changed();
    if (disconnect) socket_.abort();
    maybeClose();
}
void VoiceClient::requestClose() {
    closing_ = true;
    clearAudio(); music_.requestClose();
    if (!busy_ || !connected()) { busy_ = false; music_.releaseVoice(); maybeClose(); }
    else cancel();
}
void VoiceClient::maybeClose() {
    if (closing_ && !busy_ && music_.isClosed()) emit closeReady();
}
