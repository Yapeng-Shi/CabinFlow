#include "wake_engine.hpp"
#include "wav.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <filesystem>

namespace {
using Event = WakeEngine::Event;
void require(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }
std::vector<float> read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(input.good(), "fixture missing");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto decoded = cabinflow::agent::inference::detail::DecodeAsrWav(bytes);
    require(static_cast<bool>(decoded), "fixture must be16kHz monoPCM16");
    return std::move(decoded.value);
}
std::vector<WakeEngine::Result> feed(WakeEngine& engine, const std::vector<float>& audio) {
    std::vector<WakeEngine::Result> results;
    for (std::size_t offset = 0; offset < audio.size(); offset += WakeEngine::kFrameSamples) {
        std::array<float, WakeEngine::kFrameSamples> frame{};
        std::copy_n(audio.data() + offset, std::min(frame.size(), audio.size() - offset), frame.data());
        auto result = engine.accept(frame.data(), static_cast<int>(frame.size()), [] { return 0; });
        if (result.event != Event::kNone) results.push_back(std::move(result));
    }
    return results;
}
int count(const std::vector<WakeEngine::Result>& results, Event event) {
    return static_cast<int>(std::count_if(results.begin(), results.end(), [event](const auto& result) { return result.event == event; }));
}
void wake(WakeEngine& engine, const std::vector<float>& audio) {
    const auto results = feed(engine, audio);
    for (const auto& result : results) std::cout << "wake event=" << static_cast<int>(result.event) << " error=" << result.error << '\n';
    require(count(results, Event::kWake) == 1 && count(results, Event::kCommand) == 0 && count(results, Event::kFailed) == 0,
            "exactly one realKWS wake, no wake-word command submission");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: wake_engine_test MODEL_DIR FIXTURE_DIR NEW_OUTPUT_DIR");
        const std::filesystem::path fixtures(argv[2]), output(argv[3]);
        require(std::filesystem::create_directory(output), "output must be new");
        const auto wake_audio = read(fixtures / "wake.wav");
        const auto command = read(fixtures / "command" / "mono.wav");
        const auto negative = read(fixtures / "negative.wav");
        WakeEngine engine(argv[1]);
        require(feed(engine, negative).empty(), "unrelated speech cannot wake or submit");
        engine.reset(); wake(engine, wake_audio);
        require(engine.checkDeadline(4999).event == Event::kNone && engine.checkDeadline(5000).event == Event::kCancelled,
            "no-speech wall clock cancels at5s without WAV");
        require(feed(engine, command).empty(), "timeout cannot later submit");
        engine.reset(); wake(engine, wake_audio);
        auto results = feed(engine, command);
        require(count(results, Event::kCommand) == 0, "command without trailing silence must not finish");
        const auto short_silence = feed(engine, std::vector<float>(6400, 0));
        require(count(short_silence, Event::kCommand) == 0, "0.4s silence is shorter than approved0.8s");
        auto silence = feed(engine, std::vector<float>(WakeEngine::kSampleRate * 2, 0));
        results.insert(results.end(), std::make_move_iterator(silence.begin()), std::make_move_iterator(silence.end()));
        require(count(results, Event::kCommand) == 1 && count(results, Event::kFailed) == 0, "one VAD command after trailing silence");
        for (const auto& result : results) if (result.event == Event::kCommand) {
            const auto decoded = cabinflow::agent::inference::detail::DecodeAsrWav(result.wav);
            require(static_cast<bool>(decoded) && decoded.value.size() <= WakeEngine::kSampleRate * 15,
                    "bounded valid canonical WAV");
            std::ofstream file(output / "command.wav", std::ios::binary); file.write(result.wav.data(), static_cast<std::streamsize>(result.wav.size()));
            require(file.good(), "archive command WAV");
        }
        require(feed(engine, command).empty(), "completed engine cannot submit twice or run KWS during command");
        engine.reset(); wake(engine, wake_audio);
        auto partial = feed(engine, std::vector<float>(command.begin(), command.begin() + std::min(command.size(), std::size_t(16000))));
        require(count(partial, Event::kSpeech) == 1 && count(partial, Event::kCommand) == 0, "speech established before deadline test");
        require(engine.checkDeadline(14999).event == Event::kNone && engine.checkDeadline(15000).event == Event::kCancelled,
            "15s wallclock cancels, no truncated submission");
        engine.reset(); wake(engine, wake_audio);
        std::array<float, WakeEngine::kFrameSamples> frame{};
        int clocks = 0;
        require(engine.accept(frame.data(), static_cast<int>(frame.size()), [&] { return ++clocks == 1 ? 4999 : 5001; }).event == Event::kCancelled,
            "frame inference crossing5s loses to onset deadline, zero WAV");
        engine.reset(); wake(engine, wake_audio);
        feed(engine, std::vector<float>(command.begin(), command.begin() + std::min(command.size(), std::size_t(16000))));
        clocks = 0;
        require(engine.accept(frame.data(), static_cast<int>(frame.size()), [&] { return ++clocks == 1 ? 14999 : 15001; }).event == Event::kCancelled,
            "frame inference crossing15s loses to total deadline, zero WAV");

        // 找到 VAD 完成帧，再重放到该帧；只有第三次（编码后）读时钟才越过期限。
        auto complete_audio = command; complete_audio.resize(command.size() + 32000, 0);
        std::size_t completing = complete_audio.size();
        engine.reset(); wake(engine, wake_audio);
        for (std::size_t offset = 0; offset + frame.size() <= complete_audio.size(); offset += frame.size()) {
            std::copy_n(complete_audio.data() + offset, frame.size(), frame.data());
            if (engine.accept(frame.data(), static_cast<int>(frame.size()), [] { return 0; }).event == Event::kCommand) { completing = offset; break; }
        }
        require(completing < complete_audio.size(), "locate VAD completion frame");
        engine.reset(); wake(engine, wake_audio);
        for (std::size_t offset = 0; offset < completing; offset += frame.size()) {
            std::copy_n(complete_audio.data() + offset, frame.size(), frame.data());
            engine.accept(frame.data(), static_cast<int>(frame.size()), [] { return 0; });
        }
        std::copy_n(complete_audio.data() + completing, frame.size(), frame.data()); clocks = 0;
        require(engine.accept(frame.data(), static_cast<int>(frame.size()), [&] { return ++clocks <= 2 ? 14999 : 15001; }).event == Event::kCancelled,
            "WAV encoding crossing15s cancels before publishing success");
        engine.reset(); wake(engine, wake_audio);
        const auto no_speech = feed(engine, std::vector<float>(80000, 0));
        require(count(no_speech, Event::kCancelled) == 1 && count(no_speech, Event::kCommand) == 0, "no-speech PCM limit rejects, never submits");
        engine.reset(); wake(engine, wake_audio);
        std::vector<float> continuous;
        while (continuous.size() < 256000) continuous.insert(continuous.end(), command.begin(), command.end());
        const auto max_audio = feed(engine, continuous);
        require(count(max_audio, Event::kCancelled) == 1 && count(max_audio, Event::kCommand) == 0,
            "continuous command PCM reaches15s bound without truncated WAV");
        engine.reset(); wake(engine, wake_audio);
        require(engine.accept(nullptr, 512, [] { return 0; }).event == Event::kFailed, "invalid frame explicitly fails");
        std::cout << "wake_engine_test passed (synthetic input; human microphone unverified)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
