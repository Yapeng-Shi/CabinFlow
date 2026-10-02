#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace cabinflow::agent::inference::detail {
namespace {

uint16_t Read16(std::string_view bytes, size_t offset) {
    return static_cast<unsigned char>(bytes[offset]) |
           (static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}

uint32_t Read32(std::string_view bytes, size_t offset) {
    return static_cast<uint32_t>(Read16(bytes, offset)) |
           (static_cast<uint32_t>(Read16(bytes, offset + 2)) << 16);
}

void Append16(std::string& bytes, uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>(value >> 8));
}

void Append32(std::string& bytes, uint32_t value) {
    Append16(bytes, static_cast<uint16_t>(value));
    Append16(bytes, static_cast<uint16_t>(value >> 16));
}

BackendResult<std::vector<float>> Invalid(std::string detail) {
    return {{}, BackendError::kInvalidInput, std::move(detail)};
}

}  // namespace

BackendResult<std::vector<float>> DecodeAsrWav(std::string_view wav) {
    if (wav.size() < 12 || wav.substr(0, 4) != "RIFF" || wav.substr(8, 4) != "WAVE") {
        return Invalid("ASR requires a RIFF/WAVE container");
    }
    if (static_cast<uint64_t>(Read32(wav, 4)) + 8 != wav.size()) {
        return Invalid("WAV RIFF size does not match input bytes");
    }
    bool have_format = false;
    bool have_data = false;
    std::string_view pcm;
    size_t offset = 12;
    while (offset < wav.size()) {
        if (wav.size() - offset < 8) return Invalid("Truncated WAV chunk header");
        const auto id = wav.substr(offset, 4);
        const auto size = Read32(wav, offset + 4);
        offset += 8;
        // RIFF 奇数大小的块需要一个填充字节；先验证边界再访问，拒绝截断容器。
        const uint64_t padded = static_cast<uint64_t>(size) + (size & 1U);
        if (padded > wav.size() - offset) return Invalid("Truncated WAV chunk payload");
        if (id == "fmt ") {
            if (have_format || (size != 16 && size != 18)) {
                return Invalid("Duplicate or unsupported WAV fmt chunk");
            }
            if (Read16(wav, offset) != 1 || Read16(wav, offset + 2) != 1 ||
                Read32(wav, offset + 4) != 16000 || Read32(wav, offset + 8) != 32000 ||
                Read16(wav, offset + 12) != 2 || Read16(wav, offset + 14) != 16 ||
                (size == 18 && Read16(wav, offset + 16) != 0)) {
                return Invalid("ASR requires 16000 Hz mono PCM16 WAV");
            }
            have_format = true;
        } else if (id == "data") {
            if (!have_format || have_data || size == 0 || (size & 1U)) {
                return Invalid("Missing fmt, duplicate, empty, or misaligned WAV data");
            }
            have_data = true;
            pcm = wav.substr(offset, size);
        }
        offset += static_cast<size_t>(padded);
    }
    if (!have_format || !have_data) return Invalid("WAV requires fmt and data chunks");
    if (pcm.size() / 2 > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return Invalid("WAV exceeds ASR sample count limit");
    }
    std::vector<float> samples(pcm.size() / 2);
    for (size_t i = 0; i < samples.size(); ++i) {
        const uint16_t raw = Read16(pcm, i * 2);
        const int32_t signed_sample = raw < 32768 ? raw : static_cast<int32_t>(raw) - 65536;
        samples[i] = static_cast<float>(signed_sample) / 32768.0F;
    }
    return {std::move(samples), BackendError::kNone, {}};
}

BackendResult<WavAudio> EncodePcm16Wav(const float* samples, int32_t count, int32_t sample_rate) {
    if (!samples || count <= 0 || sample_rate <= 0 ||
        static_cast<uint64_t>(sample_rate) * 2 > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(count) * 2 + 36 > std::numeric_limits<uint32_t>::max()) {
        return {{}, BackendError::kInferenceFailed, "TTS returned invalid audio dimensions"};
    }
    const uint32_t data_size = static_cast<uint32_t>(count) * 2;
    std::string wav;
    wav.reserve(static_cast<size_t>(data_size) + 44);
    wav.append("RIFF");
    Append32(wav, data_size + 36);
    wav.append("WAVEfmt ");
    Append32(wav, 16);
    Append16(wav, 1);
    Append16(wav, 1);
    Append32(wav, static_cast<uint32_t>(sample_rate));
    Append32(wav, static_cast<uint32_t>(sample_rate) * 2);
    Append16(wav, 2);
    Append16(wav, 16);
    wav.append("data");
    Append32(wav, data_size);
    bool nonzero = false;
    for (int32_t i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) {
            return {{}, BackendError::kInferenceFailed, "TTS returned nonfinite audio samples"};
        }
        // 先限幅再量化，避免浮点模型输出超界导致 PCM16 回绕。
        const auto value = std::clamp(std::lround(std::clamp(samples[i], -1.0F, 1.0F) * 32768.0F),
                                      -32768L, 32767L);
        nonzero = nonzero || value != 0;
        Append16(wav, static_cast<uint16_t>(value));
    }
    if (!nonzero) return {{}, BackendError::kInferenceFailed, "TTS returned silent PCM16 audio"};
    return {{std::move(wav)}, BackendError::kNone, {}};
}

}  // namespace cabinflow::agent::inference::detail
