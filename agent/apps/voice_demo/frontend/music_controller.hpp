#pragma once

#include "music_backend.hpp"
#include <memory>
#include <QTimer>
#include <QVariantList>
#include <cockpit_text.pb.h>

class MusicController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY changed)
    Q_PROPERTY(bool canPrevious READ canPrevious NOTIFY changed)
    Q_PROPERTY(bool canNext READ canNext NOTIFY changed)
public:
    explicit MusicController(std::unique_ptr<MusicBackend> backend = {}, QObject* parent = nullptr);
    bool enabled() const { return static_cast<bool>(backend_); }
    bool busy() const { return phase_ != Phase::kNone || closing_; }
    QString status() const;
    QString error() const { return error_; }
    QVariantList results() const;
    QString title() const { return current_.title; }
    QString artist() const { return current_.artist; }
    bool playing() const { return snapshot_.state == MusicSnapshot::State::kPlaying; }
    bool canPlay() const { return selected_ >= 0 && current_.playable; }
    bool canPrevious() const { return selected_ > 0; }
    bool canNext() const { return selected_ >= 0 && selected_ + 1 < playlist_.size(); }
    bool isClosed() const { return closed_; }

    // 语音提交前锁住列表；终态交接前不释放，编号不会指向后来完成的新搜索。
    bool acquireVoice();
    void releaseVoice();
    void executeFromVoice(const cabinflow::agent::v1::MusicCommand& command);
    void prepareAnswer(quint64 generation);
    void stopAnswer();
    void requestClose();
    Q_INVOKABLE void search(const QString& keyword);
    Q_INVOKABLE void select(int one_based_index);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void next();
signals:
    void changed();
    void answerReady(quint64 generation);
    void answerFailed(quint64 generation, const QString& detail);
    void closeReady();
private:
    enum class Phase { kNone, kSearch, kCommand, kState, kPoll, kDuckRead, kDuckWrite,
                       kDuckConfirm, kRestoreWrite, kRestoreConfirm };
    bool admit(bool allow_during_answer = false);
    void reject(const QString& detail);
    quint64 start(Phase phase);
    void choose(const QList<MusicSong>& songs, int index);
    void readState(Phase phase);
    void commandReady(quint64 operation);
    void stateReady(quint64 operation, const MusicSnapshot& state);
    void failure(quint64 operation, const QString& detail, bool settled);
    void finish();
    void restore();
    void readyAnswer();
    void checkClose();

    std::unique_ptr<MusicBackend> backend_;
    QTimer timeout_;
    QTimer poll_;
    QList<MusicSong> results_, playlist_, pending_playlist_;
    MusicSong current_, pending_song_;
    MusicSnapshot snapshot_;
    MusicSnapshot::State expected_state_{MusicSnapshot::State::kUnknown};
    QString error_;
    Phase phase_{Phase::kNone};
    quint64 operation_{0}, answer_generation_{0};
    int selected_{-1}, pending_selected_{-1};
    bool voice_busy_{false}, failed_{false}, closing_{false}, closed_{false};
    bool answer_requested_{false}, duck_sent_{false};
    std::optional<double> baseline_;
};
