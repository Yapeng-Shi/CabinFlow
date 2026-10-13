#include "wake_engine.hpp"
#include "wav.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <QCryptographicHash>
#include <QFile>
#include <sherpa-onnx/c-api/c-api.h>

namespace {
template <class T, auto Destroy>
using Handle = std::unique_ptr<const T, decltype(Destroy)>;
constexpr char kKeyword[] = "n ǐ h ǎo x iǎo c āng @你好小舱";
}

struct WakeEngine::Impl {
    Handle<SherpaOnnxKeywordSpotter, SherpaOnnxDestroyKeywordSpotter> kws{nullptr, SherpaOnnxDestroyKeywordSpotter};
    Handle<SherpaOnnxOnlineStream, SherpaOnnxDestroyOnlineStream> stream{nullptr, SherpaOnnxDestroyOnlineStream};
    Handle<SherpaOnnxVoiceActivityDetector, SherpaOnnxDestroyVoiceActivityDetector> vad{nullptr, SherpaOnnxDestroyVoiceActivityDetector};
    bool recording{false}, speech{false}, finished{false};
    int samples{0};
    Result fail(std::string error) {
        finished = true;
        return {Event::kFailed, {}, std::move(error)};
    }
    Result cancel(std::string reason) {
        finished = true;
        return {Event::kCancelled, {}, std::move(reason)};
    }
};

WakeEngine::WakeEngine(const std::string& directory) : impl_(std::make_unique<Impl>()) {
    const std::filesystem::path root(directory);
    if (!root.is_absolute()) throw std::runtime_error("唤醒模型目录必须为绝对路径");
    const auto bundle = root / "sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01";
    const auto encoder = (bundle / "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx").string();
    const auto decoder = (bundle / "decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx").string();
    const auto joiner = (bundle / "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx").string();
    const auto tokens = (bundle / "tokens.txt").string();
    const auto silero = (root / "silero_vad_v5.onnx").string();
    for (const auto& file : {encoder, decoder, joiner, tokens, silero}) {
        if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) == 0)
            throw std::runtime_error("唤醒模型缺失或为空：" + file);
    }
    // Sherpa 的不支持模型分支可能直接 exit；进入 SDK 前验证唯一的固定文件清单。
    std::ifstream manifest(CABINFLOW_WAKE_MANIFEST);
    if (!manifest) throw std::runtime_error("唤醒模型校验清单缺失");
    for (const auto& path : {encoder, decoder, joiner, tokens, silero}) {
        std::string digest, relative;
        if (!(manifest >> digest >> relative) || (root / relative).string() != path)
            throw std::runtime_error("唤醒模型校验清单无效");
        QFile file(QString::fromStdString(path));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || hash.result().toHex().toStdString() != digest)
            throw std::runtime_error("唤醒模型 SHA-256 校验失败：" + path);
    }
    std::string unexpected;
    if (manifest >> unexpected) throw std::runtime_error("唤醒模型校验清单存在额外项");
    SherpaOnnxKeywordSpotterConfig config{};
    config.feat_config.sample_rate = kSampleRate;
    config.feat_config.feature_dim = 80;
    config.model_config.transducer.encoder = encoder.c_str();
    config.model_config.transducer.decoder = decoder.c_str();
    config.model_config.transducer.joiner = joiner.c_str();
    config.model_config.tokens = tokens.c_str();
    config.model_config.num_threads = 1;
    config.model_config.provider = "cpu";
    config.max_active_paths = 4;
    config.num_trailing_blanks = 1;
    config.keywords_score = 1;
    config.keywords_threshold = 0.25F;
    config.keywords_buf = kKeyword;
    config.keywords_buf_size = sizeof(kKeyword) - 1;
    impl_->kws.reset(SherpaOnnxCreateKeywordSpotter(&config));
    if (!impl_->kws) throw std::runtime_error("KWS 模型加载失败");

    SherpaOnnxVadModelConfig vad{};
    vad.silero_vad.model = silero.c_str();
    vad.silero_vad.threshold = 0.5F;
    vad.silero_vad.min_silence_duration = 0.8F;
    vad.silero_vad.min_speech_duration = 0.25F;
    vad.silero_vad.window_size = kFrameSamples;
    // SDK 超过此值会改阈值/静音时长；应用先在 15 秒拒绝，绝不触发该隐式分段。
    vad.silero_vad.max_speech_duration = 60;
    vad.sample_rate = kSampleRate;
    vad.num_threads = 1;
    vad.provider = "cpu";
    impl_->vad.reset(SherpaOnnxCreateVoiceActivityDetector(&vad, 16));
    if (!impl_->vad) throw std::runtime_error("Silero VAD 模型加载失败");
    reset();
}

WakeEngine::~WakeEngine() = default;
void WakeEngine::reset() {
    impl_->stream.reset(SherpaOnnxCreateKeywordStream(impl_->kws.get()));
    if (!impl_->stream) throw std::runtime_error("KWS stream 创建失败");
    SherpaOnnxVoiceActivityDetectorReset(impl_->vad.get());
    impl_->recording = impl_->speech = impl_->finished = false;
    impl_->samples = 0;
}

WakeEngine::Result WakeEngine::checkDeadline(int elapsed_ms) {
    if (!impl_->recording || impl_->finished) return {};
    if (elapsed_ms >= 15000) return impl_->cancel("录音达到 15 秒上限，未提交截断指令");
    if (!impl_->speech && elapsed_ms >= 5000) return impl_->cancel("唤醒后 5 秒未检测到语音，本轮已取消");
    return {};
}

WakeEngine::Result WakeEngine::accept(const float* samples, int count, const std::function<int()>& elapsed_ms) {
    if (impl_->finished) return {};
    if (!samples || count != kFrameSamples || !elapsed_ms) return impl_->fail("采音帧或单调时钟无效");
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 1)
            return impl_->fail("采音帧包含非法 PCM 值");
    if (!impl_->recording) {
        SherpaOnnxOnlineStreamAcceptWaveform(impl_->stream.get(), kSampleRate, samples, count);
        while (SherpaOnnxIsKeywordStreamReady(impl_->kws.get(), impl_->stream.get())) {
            SherpaOnnxDecodeKeywordStream(impl_->kws.get(), impl_->stream.get());
            Handle<SherpaOnnxKeywordResult, SherpaOnnxDestroyKeywordResult> result(
                SherpaOnnxGetKeywordResult(impl_->kws.get(), impl_->stream.get()), SherpaOnnxDestroyKeywordResult);
            if (!result) return impl_->fail("KWS 未返回有效结果");
            if (result->keyword && result->keyword[0] && std::string(result->keyword) != "你好小舱")
                return impl_->fail("KWS 返回未配置的关键词：" + std::string(result->keyword));
            if (result->keyword && std::string(result->keyword) == "你好小舱") {
                SherpaOnnxResetKeywordStream(impl_->kws.get(), impl_->stream.get());
                impl_->recording = true;
                // 只录唤醒后的指令；用户等待可见提示，不承诺“一口气唤醒并下令”。
                return {Event::kWake, {}, {}};
            }
        }
        return {};
    }
    auto wall_expired = checkDeadline(elapsed_ms());
    if (wall_expired.event != Event::kNone) return wall_expired;
    impl_->samples += count;
    const auto expired = checkDeadline(impl_->samples * 1000 / kSampleRate);
    if (expired.event != Event::kNone) return expired;
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(impl_->vad.get(), samples, count);
    // 推理可能跨过期限；必须在 speech/finished 状态提交前再次检查，而非等事件循环计时器。
    wall_expired = checkDeadline(elapsed_ms());
    if (wall_expired.event != Event::kNone) return wall_expired;
    if (!SherpaOnnxVoiceActivityDetectorEmpty(impl_->vad.get())) {
        Handle<SherpaOnnxSpeechSegment, SherpaOnnxDestroySpeechSegment> segment(
            SherpaOnnxVoiceActivityDetectorFront(impl_->vad.get()), SherpaOnnxDestroySpeechSegment);
        if (!segment || !segment->samples || segment->n <= 0 || segment->n > kSampleRate * 15)
            return impl_->fail("VAD 指令段为空或超限");
        const auto encoded = cabinflow::agent::inference::detail::EncodePcm16Wav(segment->samples, segment->n, kSampleRate);
        if (!encoded) return impl_->fail(encoded.detail);
        wall_expired = checkDeadline(elapsed_ms());
        if (wall_expired.event != Event::kNone) return wall_expired;
        impl_->finished = true;
        SherpaOnnxVoiceActivityDetectorPop(impl_->vad.get());
        return {Event::kCommand, encoded.value.bytes, {}};
    }
    if (!impl_->speech && SherpaOnnxVoiceActivityDetectorDetected(impl_->vad.get())) {
        impl_->speech = true;
        return {Event::kSpeech, {}, {}};
    }
    return {};
}
