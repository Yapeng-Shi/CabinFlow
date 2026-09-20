#pragma once

#include <cstdint>

namespace cabinflow::runtime {

class Clock {
public:
    virtual ~Clock() = default;

    [[nodiscard]] virtual std::uint64_t now_monotonic_ns() const noexcept = 0;
};

class SteadyClock final : public Clock {
public:
    [[nodiscard]] std::uint64_t now_monotonic_ns() const noexcept override;
};

}  // namespace cabinflow::runtime
