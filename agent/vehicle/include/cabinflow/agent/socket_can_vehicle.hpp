#pragma once

#include <cabinflow/agent/vehicle_backend.hpp>
#include <cabinflow/agent/vehicle_can_codec.hpp>
#include <cstdint>
#include <mutex>

namespace cabinflow::agent {

class SocketCanVehicle final : public VehicleBackend {
public:
    SocketCanVehicle() = default;
    ~SocketCanVehicle() override;
    SocketCanVehicle(const SocketCanVehicle&) = delete;
    SocketCanVehicle& operator=(const SocketCanVehicle&) = delete;

    VehicleSnapshot query(const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    VehicleSnapshot set_climate(bool on, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    VehicleSnapshot set_left_front_window(bool open, const CancelPredicate& cancelled = {}, const SubmissionGate& submit = {}) override;
    [[nodiscard]] VehicleSnapshot snapshot() const override;

private:
    VehicleSnapshot transact(vehicle_can::Operation operation, bool value, const CancelPredicate& cancelled,
                             const SubmissionGate& submit);
    bool open_socket(std::string& detail);
    void close_socket() noexcept;
    VehicleSnapshot publish(VehicleSnapshot result);

    // 一次只允许一个请求；等待 ACK 不占用状态锁，UI/终态可随时读取已确认事实。
    std::mutex operation_mutex_;
    mutable std::mutex state_mutex_;
    VehicleSnapshot state_;
    int fd_ = -1;
    std::uint32_t next_transaction_ = 1;
    bool needs_query_ = true;
};

}  // namespace cabinflow::agent
