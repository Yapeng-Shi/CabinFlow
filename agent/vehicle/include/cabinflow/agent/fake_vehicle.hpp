#pragma once

namespace cabinflow::agent {

// 具体模拟状态，无自有线程；共享访问由 VoicePipeline 的任务状态锁保护。
class FakeVehicle final {
public:
    FakeVehicle(bool initial_climate_on, bool initial_left_front_window_open) noexcept;
    [[nodiscard]] bool set_climate(bool on) noexcept;
    [[nodiscard]] bool climate_on() const noexcept;
    [[nodiscard]] bool set_left_front_window(bool open) noexcept;
    [[nodiscard]] bool left_front_window_open() const noexcept;

private:
    bool climate_on_;
    bool left_front_window_open_;
};

}  // namespace cabinflow::agent
