#pragma once

#include <optional>
#include <QList>
#include <QObject>
#include <QString>

struct MusicSong {
    QString encrypted_id, original_id, title, artist;
    bool playable{false};
};

struct MusicSnapshot {
    enum class State { kUnknown, kNoTrack, kPlaying, kPaused, kEnded };
    State state{State::kUnknown};
    QString original_id;
    std::optional<double> volume;
};

// 这是控制层的类型化异步端口，不是网易云 CLI 输出格式。真实适配器必须先通过接入门槛。
class MusicBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void search(quint64 operation, const QString& keyword) = 0;
    virtual void play(quint64 operation, const MusicSong& song) = 0;
    virtual void pause(quint64 operation) = 0;
    virtual void resume(quint64 operation) = 0;
    virtual void readState(quint64 operation) = 0;
    virtual void setVolume(quint64 operation, double volume) = 0;
    virtual void abort(quint64 operation) = 0;
    virtual void shutdown() = 0;
signals:
    void searchReady(quint64 operation, const QList<MusicSong>& songs);
    void commandReady(quint64 operation);
    void stateReady(quint64 operation, const MusicSnapshot& state);
    // settled/quiescent 必须证明旧操作不会再产生外部副作用，不是“已发送 kill”。
    void failed(quint64 operation, const QString& detail, bool settled);
    void quiescent(quint64 operation);
    // 只有自有 CLI/播放器真正退出、所有回调静止后才发送。
    void closed();
};
