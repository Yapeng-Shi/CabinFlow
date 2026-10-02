#pragma once

#include "cabinflow/agent/inference/backends.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cabinflow::agent::inference::detail {

BackendResult<std::vector<float>> DecodeAsrWav(std::string_view wav);
BackendResult<WavAudio> EncodePcm16Wav(const float* samples, int32_t count, int32_t sample_rate);

}  // namespace cabinflow::agent::inference::detail
