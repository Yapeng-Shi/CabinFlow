#include <cabinflow/agent/fake_vehicle.hpp>

namespace cabinflow::agent {

FakeVehicle::FakeVehicle(bool initial_climate_on, bool initial_left_front_window_open) noexcept
    : state_{initial_climate_on, initial_left_front_window_open, false, VehicleError::none, {}, true} {}

VehicleSnapshot FakeVehicle::query(const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return execute(std::nullopt, std::nullopt, cancelled, submit);
}

VehicleSnapshot FakeVehicle::set_climate(bool on, const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return execute(on, std::nullopt, cancelled, submit);
}

VehicleSnapshot FakeVehicle::set_left_front_window(bool open, const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return execute(std::nullopt, open, cancelled, submit);
}

VehicleSnapshot FakeVehicle::execute(std::optional<bool> climate, std::optional<bool> window,
                                    const CancelPredicate& cancelled, const SubmissionGate& submit) {
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    auto result = snapshot();
    result.action_applied = false;
    result.error = VehicleError::none;
    result.detail = "deterministic fake receipt";
    const std::function<void()> apply = [&] {
        if (climate) result.climate_on = *climate;
        if (window) result.left_front_window_open = *window;
        result.action_applied = climate.has_value() || window.has_value();
        // Fake 的执行立即确认，状态提交也必须位于取消门控临界区内。
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = result;
    };
    if ((cancelled && cancelled()) || (submit && !submit(apply))) {
        result.error = VehicleError::cancelled;
        result.detail = "cancelled before fake action";
    } else {
        if (!submit) apply();
        if (cancelled && cancelled()) {
            result.error = VehicleError::cancelled;
            result.detail = "cancelled after fake submission; action fact retained";
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(result);
    return state_;
}

VehicleSnapshot FakeVehicle::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

}  // namespace cabinflow::agent
