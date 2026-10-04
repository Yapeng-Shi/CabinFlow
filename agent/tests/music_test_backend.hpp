#pragma once
#include "music_controller.hpp"
#include <vector>

// 仅供控制层测试注入结果；不解析 CLI 文本，不启动播放器，不进入生产装配。
class TestMusicBackend final : public MusicBackend {
public:
    struct Call { QString action; quint64 id; QString value; double volume{0}; };
    std::vector<Call> calls;
    bool shutdown_requested{false};
    void search(quint64 id, const QString& keyword) override { calls.push_back({"search", id, keyword}); }
    void play(quint64 id, const MusicSong& song) override { calls.push_back({"play", id, song.original_id}); }
    void pause(quint64 id) override { calls.push_back({"pause", id, {}}); }
    void resume(quint64 id) override { calls.push_back({"resume", id, {}}); }
    void readState(quint64 id) override { calls.push_back({"state", id, {}}); }
    void setVolume(quint64 id, double volume) override { calls.push_back({"volume", id, {}, volume}); }
    void abort(quint64 id) override { calls.push_back({"abort", id, {}}); }
    void shutdown() override { shutdown_requested = true; }
    void acknowledge() { emit commandReady(calls.back().id); }
    void state(MusicSnapshot::State state, QString song = "1", double volume = 60) {
        emit stateReady(calls.back().id, {state, std::move(song), volume});
    }
    void found(const QList<MusicSong>& songs) { emit searchReady(calls.back().id, songs); }
};
