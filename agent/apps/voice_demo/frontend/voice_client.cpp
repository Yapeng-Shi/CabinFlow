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
#include <cabinflow/protocol/message_codec.hpp>
#include <cabinflow/gateway/message_limits.hpp>

namespace {
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
std::string unique_id() { return QUuid::createUuid().toString(QUuid::Id128).toStdString(); }
}

VoiceClient::VoiceClient(QString host, quint16 port, MusicController& music, WakeInput& wake, QObject* parent)
    : QObject(parent), host_(std::move(host)), port_(port), music_(music), wake_(wake) {
    connect(this, &VoiceClient::changed, this, &VoiceClient::syncWake);
    connect(&music_, &MusicController::changed, this, &VoiceClient::syncWake);
    connect(&wake_, &WakeInput::listening, this, [this](quint64 epoch, const QString& device) {
        if (epoch != wake_epoch_ || !wake_listening_ || closing_) return;
        wake_status_ = "正在监听：你好，小舱（" + device + "）"; emit changed();
    });
    connect(&wake_, &WakeInput::awake, this, [this](quint64 epoch) {
        if (epoch != wake_epoch_ || !wake_listening_ || !wake_enabled_ || busy() ||
            closing_ || !connected() || answer_active_ || music_.busy()) return;
        if (!music_.acquireVoice()) { stopWake(); emit changed(); return; }
        recording_ = true;
        wake_status_ = "已唤醒，请说指令（5 秒内开口）";
        clearAudio();
        emit changed();
    });
    connect(&wake_, &WakeInput::speech, this, [this](quint64 epoch) {
        if (epoch != wake_epoch_ || !recording_ || closing_) return;
        wake_status_ = "正在录音；静音 0.8 秒结束"; emit changed();
    });
    connect(&wake_, &WakeInput::wavReady, this, [this](quint64 epoch, const QByteArray& wav) {
        if (epoch != wake_epoch_ || !recording_ || !wake_listening_ || closing_ || !connected()) return;
        // 真实 worker 已停采音。先使重复事件失效并交接 reservation，再允许同步通知重入。
        ++wake_epoch_; wake_listening_ = false; recording_ = false; preparing_ = true;
        cabinflow::agent::v1::AudioInput input;
        input.set_wav_bytes(wav.constData(), static_cast<std::size_t>(wav.size()));
        prepared_ = PreparedInput{true, input.SerializeAsString(), false};
        startPrepared();
    });
    connect(&wake_, &WakeInput::failed, this, [this](quint64 epoch, const QString& detail) {
        if (epoch != wake_epoch_ || !wake_listening_ || closing_) return;
        wake_error_ = detail; wake_enabled_ = false;
        discardLocalInput(); stopWake(); emit changed();
    });
    connect(&wake_, &WakeInput::cancelled, this, [this](quint64 epoch, const QString& reason) {
        if (epoch != wake_epoch_ || !recording_ || closing_) return;
        wake_error_ = reason;
        discardLocalInput(); stopWake(); emit changed();
    });
    connect(&wake_, &WakeInput::settled, this, [this](quint64 epoch) {
        if (epoch != wake_epoch_ || !capture_stopping_) return;
        capture_stopping_ = false;
        if (recording_) { recording_ = false; music_.releaseVoice(); }
        if (preparing_ && !closing_ && connected()) startPrepared();
        else if (pending_play_ && !closing_ && connected()) { pending_play_ = false; play(); }
        else { pending_play_ = false; emit changed(); }
        maybeClose();
    });
    connect(&wake_, &WakeInput::closed, this, [this] {
        capture_stopping_ = false;
        if (recording_ || preparing_) music_.releaseVoice();
        recording_ = preparing_ = pending_play_ = false; prepared_.reset(); maybeClose();
    });
    player_.setAudioOutput(&audio_output_);
    connect(&grpc_, &GrpcClient::connectedToBackend, this, [this] { status_ = "就绪"; emit changed(); });
    connect(&grpc_, &GrpcClient::disconnectedFromBackend, this, [this] {
        const bool completed_local_close = closing_ && !busy_;
        discardLocalInput(); stopWake();
        clearAudio();
        controls_.clear();
        busy_ = false;
        if (!recording_) music_.releaseVoice();
        if (!completed_local_close) {
            vehicle_known_ = false;
            vehicle_action_ = "断连，执行状态未知";
            status_ = "已断连";
            const QString detail = "连接已断开；后端任务状态未知，不自动重连";
            error_ = error_.isEmpty() ? detail : error_ + "；" + detail;
        }
        emit changed();
        maybeClose();
    });
    connect(&grpc_, &GrpcClient::failed, this, [this](const QString& detail) { fail(detail); });
    connect(&grpc_, &GrpcClient::messageReceived, this, [this](const QByteArray& bytes) {
        const auto decoded = cabinflow::protocol::decode_message(bytes.toStdString());
        if (!decoded) { fail("非法 gRPC RuntimeMessage", true); return; }
        receive(decoded.message);
    });
    connect(&player_, &QMediaPlayer::playbackStateChanged, this, [this](auto state) {
        if (clearing_audio_) return;
        if (state == QMediaPlayer::StoppedState) {
            ++answer_generation_; answer_pending_ = answer_active_ = false; music_.stopAnswer();
        }
        emit changed();
    });
    connect(&player_, &QMediaPlayer::errorOccurred, this, [this](auto, const QString& detail) {
        error_ = "播放失败：" + detail;
        ++answer_generation_; answer_pending_ = answer_active_ = false; music_.stopAnswer();
        emit changed();
    });
    connect(&music_, &MusicController::answerReady, this, [this](quint64 generation) {
        if (generation != answer_generation_ || !answer_pending_ || busy_ || closing_ || !audio_file_) return;
        answer_pending_ = false; player_.play();
    });
    connect(&music_, &MusicController::answerFailed, this, [this](quint64 generation, const QString& detail) {
        if (generation != answer_generation_ || !answer_pending_) return;
        answer_pending_ = answer_active_ = false; error_ = detail; emit changed();
    });
    connect(&music_, &MusicController::closeReady, this, &VoiceClient::maybeClose);
}

VoiceClient::~VoiceClient() {
    // 成员析构仍可能触发信号；先断开回调，再停播放器/解除文件，避免访问已销毁的状态。
    grpc_.disconnect(this);
    player_.disconnect(this);
    music_.disconnect(this);
    wake_.disconnect(this);
    wake_.requestClose();
    clearAudio();
    grpc_.stop();
}

void VoiceClient::connectBackend() {
    if (busy() || closing_ || !grpc_.settled()) return;
    controls_.clear();
    vehicle_known_ = false;
    vehicle_action_ = "尚未收到模拟执行回执";
    status_ = "连接中";
    error_.clear();
    emit changed();
    // 专属 gRPC 线程处理 CQ，响应只以 queued signal 回到 GUI。
    grpc_.connectToHost(host_, port_);
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
    const auto encoded = cabinflow::protocol::encode_message(message);
    if (!encoded || encoded.bytes.size() > cabinflow::gateway::kMaxMessageBytes || !connected()) {
        fail("消息超限、无效或连接未建立"); return false;
    }
    if (!grpc_.send(QByteArray(encoded.bytes.data(), static_cast<qsizetype>(encoded.bytes.size())))) {
        fail("gRPC 发送队列拒绝消息", true); return false;
    }
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

void VoiceClient::refreshVehicleState() {
    cabinflow::agent::v1::VehicleStatusRequest request;
    begin(false, request.SerializeAsString(), true);
}

void VoiceClient::begin(bool audio, std::string payload, bool status_query) {
    if (busy() || closing_ || !connected()) return;
    if (answer_active_) { error_ = "请先停止回答播放，再开始任务"; emit changed(); return; }
    if (music_.busy()) { error_ = "音乐操作尚未静止，请稍后开始语音任务"; emit changed(); return; }
    if (!music_.acquireVoice()) { error_ = "等待音乐音量恢复后再开始任务"; emit changed(); return; }
    preparing_ = true; prepared_ = PreparedInput{audio, std::move(payload), status_query};
    clearAudio();
    if (!preparing_ || closing_) return;
    if (wake_listening_) { stopWake(); emit changed(); }
    else startPrepared();
}

void VoiceClient::startPrepared() {
    if (!preparing_ || !prepared_ || closing_ || !connected()) return;
    auto input = std::move(*prepared_); prepared_.reset();
    // 本地录音/准备态不是已准入 work；只有从这里开始才让旧响应的 busy_ 检查生效。
    preparing_ = false; busy_ = true;
    transcript_.clear(); answer_.clear(); error_.clear();
    session_ = unique_id(); trace_ = unique_id(); work_.clear(); input_id_.clear();
    audio_input_ = input.audio; payload_ = std::move(input.payload);
    status_query_ = input.status_query;
    unit_ = audio_input_ ? "asr.primary" : "dialogue.primary";
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
            auto input = envelope(status_query_ ? "cockpit.vehicle.status.request" : audio_input_ ? "cockpit.audio.input" : "cockpit.text.input", unit_);
            input.payload = std::move(payload_);
            input_id_ = input.envelope.message_id;
            const auto frame = cabinflow::protocol::encode_message(input);
            if (!frame || frame.bytes.size() > cabinflow::gateway::kMaxMessageBytes) { error_ = "完整输入编码帧超限"; cleanup_rejection_ = true; sendExit(true); return; }
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
            result.vehicle().has_climate_on() != result.vehicle().has_left_front_window_open() ||
            e.kind != ((result.has_audio() || result.has_music_command() || result.has_vehicle_status()) ? MessageKind::kData : MessageKind::kError)) {
            fail("任务终态关联/类型无效", true); return;
        }
        // 完整业务身份先校验；非法音频不能在 fail 的 changed 信号中短暂暴露伪造车控状态。
        if (result.has_audio() && (result.audio().request_message_id() != input_id_ || result.audio().wav_bytes().empty())) {
            fail("回答音频关联无效", true); return;
        }
        if (result.has_music_command() && !cabinflow::agent::is_valid_music_command(result.music_command())) {
            fail("音乐指令类型或参数无效", true); return;
        }
        if ((result.has_vehicle_status() && (!status_query_ || !result.vehicle().has_climate_on())) ||
            (status_query_ && (result.has_audio() || result.has_music_command()))) {
            fail("车辆刷新终态与请求不一致", true); return;
        }
        // 任何外部信号/播放器调用前一次性消费当前终态，重复响应不能重放音乐动作。
        terminal_consumed_ = true;
        transcript_ = QString::fromStdString(result.transcript());
        // 执行事实先于音频处理：取消、TTS 失败或本地播放器错误都不抹掉已执行的动作。
        vehicle_known_ = result.vehicle().has_climate_on() && result.vehicle().has_left_front_window_open();
        if (vehicle_known_) {
            climate_on_ = result.vehicle().climate_on();
            left_front_window_open_ = result.vehicle().left_front_window_open();
        }
        vehicle_action_ = !result.vehicle().has_action_applied() ? "本次动作：执行结果未知，需刷新车辆状态"
            : result.vehicle().action_applied() ? "本次动作：已执行（虚拟 ECU）" : "本次动作：未执行";
        if (result.has_vehicle_status()) {
            answer_.clear(); clearAudio(); terminal(vehicle_known_ ? "车辆状态已刷新" : "车辆状态未知");
        } else if (result.has_music_command()) {
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
    if (recording_ || preparing_ || pending_play_) {
        discardLocalInput(); stopWake(); stopPlayback(); emit changed(); return;
    }
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
    if (!busy() && audio_file_ && !closing_ && !answer_active_) {
        if (wake_listening_) { pending_play_ = true; stopWake(); emit changed(); return; }
        answer_active_ = true;
        answer_pending_ = true;
        music_.prepareAnswer(++answer_generation_);
        emit changed();
    }
}
void VoiceClient::stopPlayback() {
    pending_play_ = false;
    ++answer_generation_; answer_pending_ = answer_active_ = false;
    music_.stopAnswer(); player_.stop(); emit changed();
}
void VoiceClient::clearAudio() {
    clearing_audio_ = true;
    ++answer_generation_; answer_pending_ = answer_active_ = false; music_.stopAnswer();
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
    if (disconnect) { wake_enabled_ = false; grpc_.stop(); }
    discardLocalInput(); stopWake();
    clearAudio(); answer_.clear(); error_ = std::move(detail); busy_ = false; status_ = "失败";
    if (!recording_) music_.releaseVoice();
    emit changed();
    maybeClose();
}
void VoiceClient::requestClose() {
    closing_ = true;
    wake_enabled_ = false; discardLocalInput(); stopWake(); wake_.requestClose();
    clearAudio(); music_.requestClose();
    if (!busy_ || !connected()) { busy_ = false; music_.releaseVoice(); maybeClose(); }
    else cancel();
}
void VoiceClient::maybeClose() {
    if (closing_ && !busy_ && wake_.isClosed() && music_.isClosed()) {
        if (!grpc_.settled()) { grpc_.stop(); return; }
        emit closeReady();
    }
}

void VoiceClient::setWakeEnabled(bool enabled) {
    if (closing_) return;
    wake_enabled_ = enabled;
    if (enabled) wake_error_.clear();
    else { discardLocalInput(); stopWake(); }
    emit changed();
}
void VoiceClient::discardLocalInput() {
    if (preparing_) { preparing_ = false; prepared_.reset(); music_.releaseVoice(); }
    pending_play_ = false;
    // recording_ 的锁一直留到停止回执；清除录音提交权限靠 epoch，而非提前释放锁。
}
void VoiceClient::stopWake() {
    if (!wake_listening_) return;
    wake_listening_ = false; capture_stopping_ = true;
    const auto epoch = ++wake_epoch_;
    wake_status_ = "麦克风停止中";
    wake_.stop(epoch);
}
void VoiceClient::syncWake() {
    if (recording_ || capture_stopping_) return;
    const auto previous = wake_status_;
    const bool allowed = wake_enabled_ && !closing_ && connected() && !busy() &&
        !answer_active_ && !music_.busy();
    if (allowed && !wake_listening_) {
        wake_listening_ = true;
        wake_status_ = "正在启动本地监听";
        wake_.listen(++wake_epoch_);
    } else if (!allowed && wake_listening_) stopWake();
    if (!wake_listening_ && !capture_stopping_)
        wake_status_ = wake_enabled_ ? "监听暂停（任务、播放或连接未就绪）" : "麦克风已关闭";
    if (wake_status_ != previous) emit changed();
}
