#pragma once

#include <cabinflow/agent/vehicle_backend.hpp>
#include <mutex>

namespace cabinflow::agent {

// 仅供确定性测试；状态锁属于后端，调用者不能依赖流水线锁保护它。
class FakeVehicle final : public VehicleBackend {
public:
    FakeVehicle(bool initial_climate_on, bool initial_left_front_window_open) noexcept;
    VehicleSnapshot query(const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    VehicleSnapshot set_climate(bool on, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    VehicleSnapshot set_left_front_window(bool open, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    [[nodiscard]] VehicleSnapshot snapshot() const override;

private:
    VehicleSnapshot execute(std::optional<bool> climate, std::optional<bool> window,
                            const CancelPredicate& cancelled, const SubmissionGate& submit);
    std::mutex operation_mutex_;
    mutable std::mutex mutex_;
    VehicleSnapshot state_;
};

}  // namespace cabinflow::agent
