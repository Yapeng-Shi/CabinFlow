#include <cabinflow/runtime/clock.hpp>

#include <chrono>

namespace cabinflow::runtime {

std::uint64_t SteadyClock::now_monotonic_ns() const noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace cabinflow::runtime
