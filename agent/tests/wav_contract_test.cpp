#include "wav.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace cabinflow::agent::inference;
using namespace cabinflow::agent::inference::detail;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put_u32(std::string& bytes, std::size_t offset, unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 0xff);
}

void contracts() {
    const float samples[] = {0.5f, -0.5f, 0.125f};
    auto encoded = EncodePcm16Wav(samples, 3, 16000);
    require(static_cast<bool>(encoded), "encode non-silent PCM16");
    const auto decoded = DecodeAsrWav(encoded.value.bytes);
    require(decoded && decoded.value.size() == 3 && std::abs(decoded.value[0] - 0.5f) < 0.001f,
            "16k mono PCM16 round trip");
    require(!DecodeAsrWav(encoded.value.bytes.substr(0, 20)), "truncated header rejected");
    require(!DecodeAsrWav(encoded.value.bytes.substr(0, encoded.value.bytes.size() - 1)),
            "truncated body rejected");
    auto malformed = encoded.value.bytes;
    malformed[22] = 2;
    require(!DecodeAsrWav(malformed), "stereo cannot silently become mono");
    malformed = encoded.value.bytes;
    put_u32(malformed, 40, 0xffffffffu);
    require(!DecodeAsrWav(malformed), "chunk length overflow rejected");
    malformed = encoded.value.bytes;
    // RIFF 中重复 fmt 虽然边界完整，也不能产生歧义解释。
    malformed.insert(36, malformed.substr(12, 24));
    put_u32(malformed, 4, static_cast<unsigned>(malformed.size() - 8));
    require(!DecodeAsrWav(malformed), "duplicate format rejected");
    const auto tts_rate = EncodePcm16Wav(samples, 3, 44100);
    require(tts_rate && !DecodeAsrWav(tts_rate.value.bytes), "ASR rejects non-16k input; no runtime resampling");
    const float silent[] = {0.f, 0.f};
    const float invalid[] = {std::numeric_limits<float>::quiet_NaN()};
    require(!EncodePcm16Wav(silent, 2, 16000), "silent synthesis rejected");
    require(!EncodePcm16Wav(invalid, 1, 16000), "non-finite synthesis rejected");
    require(!EncodePcm16Wav(samples, 0, 16000), "empty synthesis rejected");
}
}

int main() {
    try {
        contracts();
        std::cout << "WAV format contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
