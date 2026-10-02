#include <cabinflow/agent/inference/native_backends.hpp>
#include <cabinflow/agent/voice_pipeline.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string read_wav(const std::string& path) {
    const auto size = std::filesystem::file_size(path);
    if (size == 0 || size > 4 * 1024 * 1024) throw std::runtime_error("input WAV size unsupported");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input WAV");
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (file.bad() || bytes.size() != size) throw std::runtime_error("input WAV read failed");
    return bytes;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 7 || (std::string_view(argv[1]) != "--text" && std::string_view(argv[1]) != "--wav")) {
            std::cerr << "usage: voice_demo (--text TEXT | --wav INPUT.wav) OUTPUT_DIRECTORY ASR_MODEL_DIR LLM_GGUF TTS_MODEL_DIR\n";
            return 2;
        }
        // 全新目录保证不会把历史 WAV 当作本次成功；模型由装配入口选择唯一实现。
        const std::filesystem::path output(argv[3]);
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("output directory must be new");
        const bool wav_mode = std::string_view(argv[1]) == "--wav";
        auto input = wav_mode ? read_wav(argv[2]) : std::string(argv[2]);
        cabinflow::agent::VoicePipeline pipeline(
            std::make_unique<cabinflow::agent::inference::SherpaAsrBackend>(argv[4]),
            std::make_unique<cabinflow::agent::inference::LlamaBackend>(argv[5], 2, 2048, 128),
            std::make_unique<cabinflow::agent::inference::SherpaTtsBackend>(argv[6]));
        const auto result = wav_mode ? pipeline.run_wav({std::move(input)}) : pipeline.run_text(std::move(input));
        std::ofstream report(output / "result.txt");
        report << "status=" << static_cast<int>(result.status) << "\ntrace_id=" << result.trace_id
               << "\nsession_id=" << result.session_id << "\nwork_id=" << result.work_id
               << "\nrequest_message_id=" << result.request_message_id << "\ntranscript=" << result.transcript
               << "\nanswer=" << result.answer << "\nerror=" << result.detail
               << "\nsemantic_quality=not_automatically_verified\nhuman_listening=not_verified\n";
        report.close();
        if (!report) throw std::runtime_error("cannot write result report");
        if (result.status != cabinflow::agent::VoiceStatus::kCompleted) {
            std::cerr << "task failed: " << result.detail << '\n';
            return 1;
        }
        std::ofstream audio(output / "answer.wav", std::ios::binary);
        audio.write(result.audio.bytes.data(), static_cast<std::streamsize>(result.audio.bytes.size()));
        audio.close();
        if (!audio) throw std::runtime_error("cannot write generated audio");
        std::cout << "transcript=" << result.transcript << "\nanswer=" << result.answer
                  << "\nexecution=completed output=" << (output / "answer.wav").string()
                  << "\nsemantic_quality=separate human_listening=not_verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "voice_demo failed: " << error.what() << '\n';
        return 1;
    }
}
