#include "music_controller.hpp"

#include <cmath>
#include <utility>
#include <cabinflow/agent/music_command.hpp>

namespace {
bool validSnapshot(const MusicSnapshot& state) {
    using State = MusicSnapshot::State;
    if (state.state == State::kUnknown) return false;
    if (state.state == State::kNoTrack) return state.original_id.isEmpty();
    if (state.state != State::kPlaying && state.state != State::kPaused && state.state != State::kEnded) return false;
    return !state.original_id.isEmpty() && state.volume && std::isfinite(*state.volume) &&
           *state.volume >= 0 && *state.volume <= 100;
}
}

MusicController::MusicController(std::unique_ptr<MusicBackend> backend, QObject* parent)
    : QObject(parent), backend_(std::move(backend)) {
    timeout_.setSingleShot(true);
    timeout_.setInterval(20'000);
    if (!backend_) return;
    poll_.setInterval(1'000);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!busy() && !voice_busy_ && !answer_requested_ && canPlay() &&
            snapshot_.state != MusicSnapshot::State::kUnknown) readState(Phase::kPoll);
    });
    poll_.start();
    connect(&timeout_, &QTimer::timeout, this, [this] {
        const auto id = operation_;
        failure(id, "音乐操作超时，等待外部操作静止；实际状态未知", false);
        backend_->abort(id);
    });
    connect(backend_.get(), &MusicBackend::searchReady, this, [this](quint64 id, const QList<MusicSong>& songs) {
        if (id != operation_ || phase_ != Phase::kSearch || failed_) return;
        auto list = songs.mid(0, 10);
        for (const auto& song : list) {
            if (song.title.isEmpty() || (song.playable && (song.encrypted_id.isEmpty() || song.original_id.isEmpty()))) {
                failure(id, "音乐搜索结果身份或标题无效", true); return;
            }
        }
        results_ = std::move(list);
        finish();
    });
    connect(backend_.get(), &MusicBackend::commandReady, this, &MusicController::commandReady);
    connect(backend_.get(), &MusicBackend::stateReady, this, &MusicController::stateReady);
    connect(backend_.get(), &MusicBackend::failed, this, &MusicController::failure);
    connect(backend_.get(), &MusicBackend::quiescent, this, [this](quint64 id) {
        if (id == operation_ && failed_) finish();
    });
    connect(backend_.get(), &MusicBackend::closed, this, [this] {
        if (!closing_) return;
        timeout_.stop();
        poll_.stop();
        phase_ = Phase::kNone;
        baseline_.reset(); answer_requested_ = false;
        closed_ = true;
        emit changed(); emit closeReady();
    });
}

QString MusicController::status() const {
    if (!enabled()) return "在线音乐未启用";
    if (closing_) return closed_ ? "音乐已清理" : "音乐清理中";
    if (busy()) return failed_ ? "状态未知，等待操作静止" : "执行中";
    switch (snapshot_.state) {
    case MusicSnapshot::State::kPlaying: return "播放中";
    case MusicSnapshot::State::kPaused: return "暂停";
    case MusicSnapshot::State::kEnded: return "结束";
    case MusicSnapshot::State::kNoTrack: return "未选择歌曲";
    default: return "状态未知";
    }
}

QVariantList MusicController::results() const {
    QVariantList list;
    for (int i = 0; i < results_.size(); ++i) list.append(QVariantMap{
        {"index", i + 1}, {"title", results_[i].title}, {"artist", results_[i].artist},
        {"playable", results_[i].playable}});
    return list;
}

void MusicController::reject(const QString& detail) { error_ = detail; emit changed(); }
bool MusicController::admit(bool allow_during_answer) {
    if (!enabled()) { reject("在线音乐未启用：等待官方授权和播放器接入验证"); return false; }
    if (busy() || voice_busy_) { reject("语音或音乐操作尚未完成，不积压新命令"); return false; }
    if (answer_requested_ && !allow_during_answer) { reject("请先停止回答播放，再切换或恢复音乐"); return false; }
    error_.clear(); return true;
}
quint64 MusicController::start(Phase phase) {
    phase_ = phase; failed_ = false;
    timeout_.start();
    return ++operation_;
}
bool MusicController::acquireVoice() {
    if (busy() || voice_busy_ || answer_requested_) return false;
    voice_busy_ = true; return true;
}
void MusicController::releaseVoice() { voice_busy_ = false; }

void MusicController::executeFromVoice(const cabinflow::agent::v1::MusicCommand& command) {
    if (!voice_busy_) { reject("音乐指令没有当前语音任务所有权"); return; }
    // 同一 GUI 调用栈完成锁的交接；不能先通知 UI 再按当前列表解释编号。
    voice_busy_ = false;
    if (!cabinflow::agent::is_valid_music_command(command)) { reject("音乐指令类型或参数无效"); return; }
    using Command = cabinflow::agent::v1::MusicCommand;
    switch (command.action()) {
    case Command::SEARCH: search(QString::fromStdString(command.keyword())); break;
    case Command::SELECT:
        if (command.result_index() > 10) reject("歌曲编号超出当前搜索列表");
        else select(static_cast<int>(command.result_index()));
        break;
    case Command::PLAY: play(); break;
    case Command::PAUSE: pause(); break;
    case Command::PREVIOUS: previous(); break;
    case Command::NEXT: next(); break;
    default: reject("不支持的音乐指令"); break;
    }
}

void MusicController::search(const QString& keyword) {
    if (!admit(true)) return;
    if (keyword.trimmed().isEmpty() || keyword.toUtf8().size() > 16 * 1024) { reject("搜索关键词为空或超限"); return; }
    // 搜索失败后不能让新编号继续指向旧结果；已冻结的播放列表独立保留。
    results_.clear();
    const auto id = start(Phase::kSearch);
    backend_->search(id, keyword.trimmed()); emit changed();
}
void MusicController::choose(const QList<MusicSong>& songs, int index) {
    if (index < 0 || index >= songs.size()) { reject("歌曲编号超出当前列表"); return; }
    if (!songs[index].playable) { reject("该歌曲当前账号不可播放"); return; }
    pending_playlist_ = songs; pending_selected_ = index; pending_song_ = songs[index];
    expected_state_ = MusicSnapshot::State::kPlaying;
    const auto id = start(Phase::kCommand);
    backend_->play(id, pending_song_); emit changed();
}
void MusicController::select(int index) {
    if (!admit()) return;
    if (index <= 0 || index > results_.size()) { reject("歌曲编号超出当前搜索列表"); return; }
    choose(results_, index - 1);
}
void MusicController::previous() { if (admit()) choose(playlist_, selected_ - 1); }
void MusicController::next() { if (admit()) choose(playlist_, selected_ + 1); }
void MusicController::play() {
    if (!admit()) return;
    if (!canPlay()) { reject("请先搜索并选择歌曲"); return; }
    if (snapshot_.state == MusicSnapshot::State::kPaused) {
        pending_song_ = current_; pending_selected_ = selected_; pending_playlist_ = playlist_;
        expected_state_ = MusicSnapshot::State::kPlaying;
        const auto id = start(Phase::kCommand); backend_->resume(id); emit changed();
    } else choose(playlist_, selected_);
}
void MusicController::pause() {
    if (!admit(true)) return;
    if (!canPlay()) { reject("尚未选择歌曲"); return; }
    pending_song_ = current_; pending_selected_ = selected_; pending_playlist_ = playlist_;
    expected_state_ = MusicSnapshot::State::kPaused;
    const auto id = start(Phase::kCommand); backend_->pause(id); emit changed();
}
void MusicController::readState(Phase phase) { const auto id = start(phase); backend_->readState(id); }
void MusicController::commandReady(quint64 id) {
    if (id != operation_ || failed_ || closing_) return;
    if (phase_ == Phase::kCommand) readState(Phase::kState);
    else if (phase_ == Phase::kDuckWrite) readState(Phase::kDuckConfirm);
    else if (phase_ == Phase::kRestoreWrite) readState(Phase::kRestoreConfirm);
}
void MusicController::stateReady(quint64 id, const MusicSnapshot& state) {
    if (id != operation_ || failed_ || closing_) return;
    if (phase_ != Phase::kState && phase_ != Phase::kPoll && phase_ != Phase::kDuckRead &&
        phase_ != Phase::kDuckConfirm && phase_ != Phase::kRestoreConfirm) return;
    if (!validSnapshot(state)) { failure(id, "播放器状态或音量回读无效", true); return; }
    if (phase_ != Phase::kState && state.state != MusicSnapshot::State::kNoTrack &&
        (current_.original_id.isEmpty() || state.original_id != current_.original_id)) {
        failure(id, "播放器回读不是当前自有歌曲，状态未知", true); return;
    }
    if (phase_ == Phase::kPoll) {
        if (state.state != MusicSnapshot::State::kNoTrack && state.original_id != current_.original_id) {
            failure(id, "播放器歌曲身份发生非预期变化", true); return;
        }
        snapshot_ = state; finish(); return;
    }
    if (phase_ == Phase::kState) {
        if (state.original_id != pending_song_.original_id) {
            failure(id, "播放器回读的歌曲与请求不同，状态未知", true); return;
        }
        if (state.state != expected_state_ && state.state != MusicSnapshot::State::kEnded) {
            failure(id, "播放器没有确认请求的播放状态", true); return;
        }
        playlist_ = pending_playlist_; selected_ = pending_selected_; current_ = pending_song_;
        snapshot_ = state; finish(); return;
    }
    if (phase_ == Phase::kDuckRead) {
        snapshot_ = state;
        if (!answer_requested_) { finish(); return; }
        if (state.state != MusicSnapshot::State::kPlaying) { readyAnswer(); return; }
        baseline_ = state.volume;
        duck_sent_ = true;
        const auto volume_id = start(Phase::kDuckWrite);
        backend_->setVolume(volume_id, *baseline_ * 0.2); return;
    }
    const bool restoring = phase_ == Phase::kRestoreConfirm;
    const double expected = restoring ? *baseline_ : *baseline_ * 0.2;
    if (!state.volume || std::abs(*state.volume - expected) > 0.01) {
        failure(id, restoring ? "音量恢复未确认，音量状态未知" : "音乐降音量未确认", true); return;
    }
    snapshot_ = state;
    if (restoring) { baseline_.reset(); duck_sent_ = false; finish(); }
    else if (answer_requested_) readyAnswer();
    else finish();
}

void MusicController::prepareAnswer(quint64 generation) {
    if (closing_ || busy() || voice_busy_ || answer_requested_) {
        emit answerFailed(generation, "音乐操作未静止，暂不能播放回答"); return;
    }
    answer_generation_ = generation; answer_requested_ = true;
    if (!backend_) { emit answerReady(generation); return; }
    error_.clear(); readState(Phase::kDuckRead); emit changed();
}
void MusicController::readyAnswer() {
    const auto generation = answer_generation_;
    finish();
    if (answer_requested_ && !closing_) emit answerReady(generation);
}
void MusicController::stopAnswer() {
    answer_requested_ = false;
    // 旧 volume 写入仍可能在外部执行；等当前事务静止后再恢复，不提前释放操作槽。
    if (phase_ == Phase::kNone && baseline_) restore();
}
void MusicController::restore() {
    const auto id = start(Phase::kRestoreWrite);
    backend_->setVolume(id, *baseline_); emit changed();
}
void MusicController::failure(quint64 id, const QString& detail, bool settled) {
    if (id != operation_ || phase_ == Phase::kNone || closing_) return;
    timeout_.stop(); failed_ = true;
    error_ = detail; snapshot_.state = MusicSnapshot::State::kUnknown;
    if (phase_ == Phase::kDuckRead || phase_ == Phase::kDuckWrite || phase_ == Phase::kDuckConfirm) {
        answer_requested_ = false;
        emit answerFailed(answer_generation_, detail);
    }
    if (phase_ == Phase::kRestoreWrite || phase_ == Phase::kRestoreConfirm) {
        // 恢复失败不重试；只有 settled/quiescent 之后才能结束音量事务。
        if (settled) { baseline_.reset(); duck_sent_ = false; }
    }
    if (settled) finish(); else emit changed();
}
void MusicController::finish() {
    const bool restore_failed = failed_ && (phase_ == Phase::kRestoreWrite || phase_ == Phase::kRestoreConfirm);
    timeout_.stop(); phase_ = Phase::kNone; failed_ = false;
    if (restore_failed) { baseline_.reset(); duck_sent_ = false; }
    if (!closing_ && !answer_requested_ && baseline_ && duck_sent_) { restore(); return; }
    emit changed(); checkClose();
}
void MusicController::requestClose() {
    if (closing_) return;
    closing_ = true; voice_busy_ = false; answer_requested_ = false; timeout_.stop(); poll_.stop();
    // Backend 的关闭合同包含所有 pending 操作与自有播放器；不靠析构隐式等待。
    if (backend_) backend_->shutdown();
    else { closed_ = true; emit closeReady(); }
    emit changed();
}
void MusicController::checkClose() {
    if (closing_ && closed_) emit closeReady();
}
