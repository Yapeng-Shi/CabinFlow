#include <cabinflow/agent/fake_vehicle.hpp>

namespace cabinflow::agent {

FakeVehicle::FakeVehicle(bool initial_climate_on, bool initial_left_front_window_open) noexcept
    : climate_on_(initial_climate_on), left_front_window_open_(initial_left_front_window_open) {}

bool FakeVehicle::set_climate(bool on) noexcept {
    climate_on_ = on;
    return climate_on_;
}

bool FakeVehicle::climate_on() const noexcept { return climate_on_; }

bool FakeVehicle::set_left_front_window(bool open) noexcept {
    left_front_window_open_ = open;
    return left_front_window_open_;
}

bool FakeVehicle::left_front_window_open() const noexcept { return left_front_window_open_; }

}  // namespace cabinflow::agent
