#pragma once
#include <cstddef>
namespace cabinflow::gateway {
inline constexpr std::size_t kMaxMessageBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxStreamOutputMessages = 8;
}
