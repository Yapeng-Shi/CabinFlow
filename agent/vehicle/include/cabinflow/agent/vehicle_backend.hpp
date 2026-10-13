#pragma once

#include <functional>
#include <optional>
#include <string>

namespace cabinflow::agent {

enum class VehicleError { none, cancelled, unavailable, invalid_response, rejected, timeout, state_unknown, io_error };

struct VehicleSnapshot {
    std::optional<bool> climate_on;
    std::optional<bool> left_front_window_open;
    std::optional<bool> action_applied;
    VehicleError error = VehicleError::none;
    std::string detail;
    bool simulated = true;
};

using CancelPredicate = std::function<bool()>;
// gate 将“检查取消”和一次非阻塞提交放进同一个调用者临界区；ACK 等待在 gate 外。
using SubmissionGate = std::function<bool(const std::function<void()>&)>;

class VehicleBackend {
public:
    virtual ~VehicleBackend() = default;
    virtual VehicleSnapshot query(const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) = 0;
    virtual VehicleSnapshot set_climate(bool on, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) = 0;
    virtual VehicleSnapshot set_left_front_window(bool open, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) = 0;
    [[nodiscard]] virtual VehicleSnapshot snapshot() const = 0;
};

[[nodiscard]] const char* vehicle_error_name(VehicleError error) noexcept;

}  // namespace cabinflow::agent
