#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QTemporaryFile>
#include <QUrl>

#include <cabinflow/protocol/message.hpp>
#include "grpc_client.hpp"
#include "music_controller.hpp"
#include "wake_input.hpp"

class VoiceClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString transcript READ transcript NOTIFY changed)
    Q_PROPERTY(QString answer READ answer NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool playbackActive READ playbackActive NOTIFY changed)
    Q_PROPERTY(bool wakeEnabled READ wakeEnabled NOTIFY changed)
    Q_PROPERTY(QString wakeStatus READ wakeStatus NOTIFY changed)
    Q_PROPERTY(QString wakeError READ wakeError NOTIFY changed)
    Q_PROPERTY(QString vehicleState READ vehicleState NOTIFY changed)
    Q_PROPERTY(QString vehicleAction READ vehicleAction NOTIFY changed)
    Q_PROPERTY(bool vehicleKnown READ vehicleKnown NOTIFY changed)
    Q_PROPERTY(bool climateOn READ climateOn NOTIFY changed)
    Q_PROPERTY(bool leftFrontWindowOpen READ leftFrontWindowOpen NOTIFY changed)
public:
    VoiceClient(QString host, quint16 port, MusicController& music, WakeInput& wake, QObject* parent = nullptr);
    ~VoiceClient() override;
    QString status() const { return status_; }
    QString transcript() const { return transcript_; }
    QString answer() const { return answer_; }
    QString error() const { return error_; }
    bool busy() const { return busy_ || recording_ || preparing_ || capture_stopping_ || pending_play_; }
    bool wakeEnabled() const { return wake_enabled_; }
    QString wakeStatus() const { return wake_status_; }
    QString wakeError() const { return wake_error_; }
    bool connected() const { return grpc_.connected(); }
    bool hasAudio() const { return static_cast<bool>(audio_file_); }
    // 仅供 GUI 线程同步读取/复制；下一任务、断连或关闭会删除文件，不转移所有权。
    QString answerAudioPath() const { return audio_file_ ? audio_file_->fileName() : QString{}; }
    bool playing() const { return player_.playbackState() == QMediaPlayer::PlayingState; }
    bool playbackActive() const { return answer_active_ || pending_play_; }
    QString vehicleState() const {
        if (!vehicle_known_) return "空调模拟：未知";
        return climate_on_ ? "空调模拟（最近回执）：开" : "空调模拟（最近回执）：关";
    }
    QString vehicleAction() const { return vehicle_action_; }
    bool vehicleKnown() const { return vehicle_known_; }
    bool climateOn() const { return climate_on_; }
    bool leftFrontWindowOpen() const { return left_front_window_open_; }

    Q_INVOKABLE void connectBackend();
    Q_INVOKABLE void startText(const QString& text);
    Q_INVOKABLE void startWav(const QUrl& file);
    Q_INVOKABLE void refreshVehicleState();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void play();
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void requestClose();
    Q_INVOKABLE void setWakeEnabled(bool enabled);
signals:
    void changed();
    void closeReady();
private:
    struct ControlPending {
        bool setup{false};
        bool cleanup_only{false};
        std::string session, work, trace, unit;
    };
    void begin(bool audio, std::string payload, bool status_query = false);
    void startPrepared();
    void syncWake();
    void stopWake();
    void discardLocalInput();
    void receive(const cabinflow::protocol::Message& message);
    void sendExit(bool cleanup_only);
    bool send(const cabinflow::protocol::Message& message);
    cabinflow::protocol::Message envelope(std::string topic, std::string target);
    void fail(QString detail, bool disconnect = false);
    void terminal(QString state);
    void clearAudio();
    void maybeClose();

    QString host_;
    quint16 port_;
    MusicController& music_;
    WakeInput& wake_;
    GrpcClient grpc_;
    QAudioOutput audio_output_;
    QMediaPlayer player_;
    // 文件由客户端持有到播放器停止、解除 source 后才删除。
    std::unique_ptr<QTemporaryFile> audio_file_;
    std::map<std::string, ControlPending> controls_;
    std::string session_, work_, trace_, input_id_, unit_, payload_;
    QString status_{"未连接"}, transcript_, answer_, error_;
    bool audio_input_{false}, busy_{false}, input_sent_{false};
    bool status_query_{false};
    bool cancel_requested_{false}, closing_{false}, cleanup_rejection_{false};
    bool terminal_consumed_{false}, answer_pending_{false}, clearing_audio_{false};
    quint64 answer_generation_{0};
    struct PreparedInput { bool audio; std::string payload; bool status_query; };
    std::optional<PreparedInput> prepared_;
    bool wake_enabled_{false}, wake_listening_{false}, recording_{false};
    bool preparing_{false}, capture_stopping_{false}, pending_play_{false}, answer_active_{false};
    quint64 wake_epoch_{0};
    QString wake_status_{"麦克风已关闭"}, wake_error_;
    // 未收到可信业务回执时不把 Protobuf 的 false 默认值当作实际“关”。
    bool vehicle_known_{false}, climate_on_{false};
    bool left_front_window_open_{false};
    QString vehicle_action_{"尚未收到模拟执行回执"};
};
