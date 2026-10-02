#pragma once

#include <atomic>
#include <cstdint>

#include <cabinflow/runtime/clock.hpp>

namespace cabinflow::test {

class FakeClock final : public runtime::Clock {
public:
    explicit FakeClock(std::uint64_t now_monotonic_ns)
        : now_monotonic_ns_(now_monotonic_ns) {}

    [[nodiscard]] std::uint64_t now_monotonic_ns() const noexcept override {
        return now_monotonic_ns_.load(std::memory_order_relaxed);
    }

    void advance(std::uint64_t elapsed_ns) noexcept {
        now_monotonic_ns_.fetch_add(elapsed_ns, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> now_monotonic_ns_;
};

}  // namespace cabinflow::test
