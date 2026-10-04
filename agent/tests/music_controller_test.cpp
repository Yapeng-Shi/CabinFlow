#include "music_test_backend.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using State = MusicSnapshot::State;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
QList<MusicSong> songs() {
    return {{"encrypted-1", "1", "测试歌曲一", "测试歌手", true},
            {"encrypted-2", "2", "测试歌曲二", "测试歌手", true},
            {{}, {}, "不可播歌曲", "测试歌手", false}};
}
struct Fixture {
    Fixture() : raw(new TestMusicBackend), controller(std::unique_ptr<MusicBackend>(raw)) {}
    void playing() {
        controller.search("test"); raw->found(songs()); controller.select(1);
        raw->acknowledge(); raw->state(State::kPlaying);
    }
    TestMusicBackend* raw;
    MusicController controller;
};
void disabled_and_admission() {
    MusicController disabled;
    disabled.search("test"); disabled.play();
    require(!disabled.enabled() && disabled.results().isEmpty() && !disabled.playing() &&
        disabled.status() == "在线音乐未启用", "unconfigured production controller never invents success");
    int ready = 0; QObject::connect(&disabled, &MusicController::answerReady, [&](quint64) { ++ready; });
    disabled.prepareAnswer(1); require(ready == 1, "ordinary TTS works without online music");
    disabled.stopAnswer();
    Fixture f; f.controller.play(); f.controller.select(std::numeric_limits<int>::min());
    require(f.raw->calls.empty(), "no selection or invalid index cannot call player");
    require(f.controller.acquireVoice(), "voice owns stable search snapshot");
    f.controller.search("blocked"); f.controller.select(1); f.controller.previous(); f.controller.next(); f.controller.pause();
    require(f.raw->calls.empty(), "C++ entrypoints reject all mutations while voice task active");
    cabinflow::agent::v1::MusicCommand command; command.set_action(command.SEARCH); command.set_keyword("voice");
    f.controller.executeFromVoice(command);
    require(f.controller.busy() && f.raw->calls.size() == 1 && f.raw->calls.back().action == "search",
            "voice terminal hands ownership directly to controller operation");
    require(!f.controller.acquireVoice(), "no voice task during unresolved music operation");
}
void playlist_and_readback() {
    Fixture f; f.controller.search("first"); const auto search_id = f.raw->calls.back().id; f.raw->found(songs());
    require(f.raw->calls.size() == 1 && f.controller.results().size() == 3 && !f.controller.playing(), "search does not auto play");
    f.controller.select(0); f.controller.select(4); f.controller.select(3);
    require(f.raw->calls.size() == 1, "invalid and unavailable choices are explicit without playback");
    f.controller.select(2); const auto play_id = f.raw->calls.back().id;
    require(!f.controller.playing() && f.controller.busy(), "issued command does not imply playing");
    f.raw->acknowledge(); require(f.controller.busy() && !f.controller.playing(), "CLI ack still waits for real state");
    f.raw->state(State::kPlaying, "2");
    require(f.controller.playing() && f.controller.title() == "测试歌曲二", "only state confirms selected song");
    emit f.raw->commandReady(play_id); emit f.raw->searchReady(search_id, {});
    require(f.controller.playing() && f.controller.results().size() == 3, "stale callbacks do not change current state");
    f.controller.search("second"); f.raw->found({{"encrypted-new", "9007199254740993", "新搜索歌曲", "另一歌手", true}});
    require(f.controller.playing(), "new search does not stop current song");
    f.controller.previous(); require(f.raw->calls.back().value == "1", "previous uses frozen playback list not new search results");
    f.raw->acknowledge(); f.raw->state(State::kPlaying);
    const auto count = f.raw->calls.size(); f.controller.previous();
    require(f.raw->calls.size() == count, "playlist boundary does not wrap");
    f.controller.select(1); require(f.raw->calls.back().value == "9007199254740993", "song IDs remain strings without precision loss");
}
void failures_and_shutdown() {
    Fixture f; f.controller.search("test"); const auto id = f.raw->calls.back().id;
    emit f.raw->failed(id, "injected timeout", false);
    f.controller.search("new"); emit f.raw->searchReady(id, songs());
    require(f.controller.busy() && f.raw->calls.size() == 1 && f.controller.results().isEmpty(),
            "timeout keeps slot until external effects are quiescent; late success ignored");
    emit f.raw->quiescent(id); require(!f.controller.busy() && f.controller.status() == "状态未知", "quiescence releases slot without success");
    f.controller.search("new"); f.raw->found({{{}, {}, {}, {}, true}});
    require(f.controller.results().isEmpty() && !f.controller.error().isEmpty(), "malformed search result rejected");
    f.playing(); f.controller.pause(); f.raw->acknowledge(); f.raw->state(State::kPlaying);
    require(!f.controller.playing() && f.controller.status() == "状态未知", "ack and wrong readback do not fake pause success");
    bool closed = false; QObject::connect(&f.controller, &MusicController::closeReady, [&] { closed = true; });
    f.controller.requestClose(); f.controller.search("blocked");
    require(f.raw->shutdown_requested && !closed, "close waits for backend cleanup proof");
    emit f.raw->closed(); require(closed && f.controller.isClosed(), "close released only after backend actually closed");
}
void failed_search_cannot_reuse_numbers() {
    Fixture f; f.playing();
    f.controller.search("new search");
    require(f.controller.results().isEmpty(), "new search invalidates old numbered results immediately");
    emit f.raw->failed(f.raw->calls.back().id, "injected provider search failure", true);
    const auto count = f.raw->calls.size(); f.controller.select(1);
    require(f.raw->calls.size() == count, "failed search cannot select historical result by new ordinal");
    f.controller.next();
    require(f.raw->calls.back().action == "play" && f.raw->calls.back().value == "2",
            "failed search does not erase separately frozen playback playlist");
}
void eof_never_auto_plays() {
    Fixture f; f.playing(); const auto count = f.raw->calls.size();
    QElapsedTimer elapsed; elapsed.start();
    while (f.raw->calls.size() == count && elapsed.elapsed() < 2500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    require(f.raw->calls.size() == count + 1 && f.raw->calls.back().action == "state", "idle timer reads actual EOF state");
    f.raw->state(State::kEnded);
    require(f.controller.status() == "结束" && !f.controller.playing() && f.raw->calls.size() == count + 1,
            "EOF updates UI without automatic next/play");
    f.controller.play(); f.raw->acknowledge(); f.raw->state(State::kPlaying);
    f.controller.prepareAnswer(9); f.raw->state(State::kPlaying); f.raw->acknowledge(); f.raw->state(State::kEnded, "1", 12);
    require(f.controller.status() == "结束", "EOF during duck readback is retained rather than resumed");
    f.controller.stopAnswer(); f.raw->acknowledge(); f.raw->state(State::kEnded, "1", 60);
    require(f.controller.status() == "结束" && !f.controller.playing() && f.raw->calls.back().action == "state",
            "EOF during restore does not generate resume or next");
}
void ducking_and_pause() {
    Fixture f; f.playing(); int ready = 0;
    QObject::connect(&f.controller, &MusicController::answerReady, [&](quint64 generation) { require(generation == 7, "answer generation retained"); ++ready; });
    f.controller.prepareAnswer(7); f.raw->state(State::kPlaying);
    require(f.raw->calls.back().action == "volume" && std::abs(f.raw->calls.back().volume - 12) < .01 && ready == 0,
            "baseline volume lowered to 20 percent before TTS");
    f.raw->acknowledge(); f.raw->state(State::kPlaying, "1", 12);
    require(ready == 1 && !f.controller.busy(), "TTS allowed only after verified duck readback");
    const auto calls = f.raw->calls.size(); f.controller.prepareAnswer(8);
    require(f.raw->calls.size() == calls, "duplicate answer start does not repeatedly multiply volume");
    f.controller.pause(); f.raw->acknowledge(); f.raw->state(State::kPaused, "1", 12);
    f.controller.stopAnswer(); require(f.raw->calls.back().volume == 60, "stop restores recorded baseline exactly");
    f.raw->acknowledge(); f.raw->state(State::kPaused, "1", 60);
    require(!f.controller.playing() && f.controller.status() == "暂停", "volume restoration never resumes paused music");
}
void stopped_duck_write_and_restore_failure() {
    Fixture f; f.playing(); int ready = 0;
    QObject::connect(&f.controller, &MusicController::answerReady, [&](quint64) { ++ready; });
    f.controller.prepareAnswer(1); f.raw->state(State::kPlaying);
    const auto old_write = f.raw->calls.back().id;
    f.controller.stopAnswer(); f.controller.prepareAnswer(2);
    require(f.controller.busy() && f.raw->calls.back().id == old_write, "stopped duck write retains transaction; new TTS cannot overtake");
    f.raw->acknowledge(); f.raw->state(State::kPlaying, "1", 12);
    require(ready == 0 && f.raw->calls.back().action == "volume" && f.raw->calls.back().volume == 60,
            "late duck ack does not play answer and triggers restore");
    const auto restore_id = f.raw->calls.back().id; emit f.raw->failed(restore_id, "restore failed", false);
    require(f.controller.busy(), "failed restore retains slot while unsettled");
    emit f.raw->quiescent(restore_id);
    require(!f.controller.busy() && f.controller.status() == "状态未知", "failed restore not retried or claimed successful");
    const auto calls = f.raw->calls.size(); emit f.raw->commandReady(old_write);
    require(f.raw->calls.size() == calls && ready == 0, "old volume completion cannot revive TTS");
}
void operation_timeout() {
    Fixture f; f.controller.search("never completes"); const auto id = f.raw->calls.back().id;
    QElapsedTimer elapsed; elapsed.start();
    while (f.raw->calls.back().action != "abort" && elapsed.elapsed() < 22'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    require(f.raw->calls.size() == 2 && f.raw->calls.back().action == "abort" &&
        f.raw->calls.back().id == id && f.controller.busy(), "actual deadline aborts once and retains unsettled operation");
    emit f.raw->searchReady(id, songs()); require(f.controller.results().isEmpty(), "post-timeout late success cannot overwrite state");
    emit f.raw->quiescent(id); require(!f.controller.busy(), "actual deadline waits for quiescence proof");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 2 && QString(argv[1]) == "--timeout") { operation_timeout(); return 0; }
        require(argc == 1, "usage: music_controller_test [--timeout]");
        disabled_and_admission(); playlist_and_readback(); failures_and_shutdown();
        failed_search_cannot_reuse_numbers(); eof_never_auto_plays();
        ducking_and_pause(); stopped_duck_write_and_restore_failure();
        std::cout << "music control contracts passed; typed test backend is not NetEase or real audio\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
