#include <cabinflow/agent/socket_can_vehicle.hpp>
#include <cabinflow/agent/vcan_interface.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cabinflow::agent {
namespace {

std::string system_error(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool is_cancelled(const CancelPredicate& cancelled) { return cancelled && cancelled(); }

}  // namespace

SocketCanVehicle::~SocketCanVehicle() { close_socket(); }

void SocketCanVehicle::close_socket() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SocketCanVehicle::open_socket(std::string& detail) {
    if (fd_ >= 0) return true;
    fd_ = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd_ < 0) {
        detail = system_error("SocketCAN socket");
        return false;
    }
    ifreq interface{};
    std::strncpy(interface.ifr_name, "vcan0", IFNAMSIZ - 1);
    if (::ioctl(fd_, SIOCGIFINDEX, &interface) < 0) {
        detail = system_error("vcan0 interface");
        close_socket();
        return false;
    }
    if (!vehicle_can::is_vcan_interface(static_cast<unsigned>(interface.ifr_ifindex), detail)) {
        close_socket();
        return false;
    }
    // mask 中的 CAN_ERR_FLAG 会选择错误帧列表，而不是排除错误帧；数据与错误过滤是独立选项。
    const can_filter filter{vehicle_can::response_id, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
    if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
        detail = system_error("SocketCAN response filter");
        close_socket();
        return false;
    }
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = interface.ifr_ifindex;
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        detail = system_error("SocketCAN vcan0 bind");
        close_socket();
        return false;
    }
    return true;
}

VehicleSnapshot SocketCanVehicle::snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

VehicleSnapshot SocketCanVehicle::publish(VehicleSnapshot result) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = std::move(result);
    return state_;
}

VehicleSnapshot SocketCanVehicle::query(const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return transact(vehicle_can::Operation::query, false, cancelled, submit);
}

VehicleSnapshot SocketCanVehicle::set_climate(bool on, const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return transact(vehicle_can::Operation::climate, on, cancelled, submit);
}

VehicleSnapshot SocketCanVehicle::set_left_front_window(bool open, const CancelPredicate& cancelled, const SubmissionGate& submit) {
    return transact(vehicle_can::Operation::left_front_window, open, cancelled, submit);
}

VehicleSnapshot SocketCanVehicle::transact(vehicle_can::Operation operation, bool value,
                                          const CancelPredicate& cancelled, const SubmissionGate& submit) {
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    auto result = snapshot();
    result.action_applied = false;
    result.error = VehicleError::none;
    result.detail.clear();
    if (is_cancelled(cancelled)) {
        result.error = VehicleError::cancelled;
        result.detail = "cancelled before CAN write";
        return publish(std::move(result));
    }
    const bool setter = operation != vehicle_can::Operation::query;
    if (setter && needs_query_) {
        result.error = VehicleError::state_unknown;
        result.detail = "explicit successful vehicle query required before setting";
        return publish(std::move(result));
    }
    if (!open_socket(result.detail)) {
        result.error = VehicleError::unavailable;
        return publish(std::move(result));
    }
    // 旧事务积压在提交锁外清理；限定非阻塞读取次数，后续仍用事务号拒绝旧 ACK。
    for (unsigned drained = 0; drained < 64; ++drained) {
        can_frame stale{};
        const auto bytes = ::read(fd_, &stale, sizeof(stale));
        if (bytes >= 0) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        result.error = VehicleError::io_error;
        result.detail = system_error("CAN drain before write");
        close_socket();
        return publish(std::move(result));
    }
    if (next_transaction_ == 0) {
        result.error = VehicleError::io_error;
        result.detail = "CAN transaction identity exhausted; backend must be recreated";
        return publish(std::move(result));
    }
    // uint32 溢出后明确拒绝，不复用仍可能存在于总线上的旧事务身份。
    const auto transaction = next_transaction_++;
    const auto request = vehicle_can::encode_request({operation, transaction, value});
    if (is_cancelled(cancelled)) {
        result.error = VehicleError::cancelled;
        result.detail = "cancelled before CAN write";
        return publish(std::move(result));
    }
    ssize_t written = -1;
    int write_error = 0;
    const std::function<void()> send = [&] {
        written = ::write(fd_, &request, sizeof(request));
        write_error = errno;
    };
    if (submit) {
        if (!submit(send)) {
            result.error = VehicleError::cancelled;
            result.detail = "cancelled at CAN submission gate; no write";
            return publish(std::move(result));
        }
    } else {
        send();
    }
    if (written != static_cast<ssize_t>(sizeof(request))) {
        result.error = VehicleError::io_error;
        result.detail = written < 0 ? std::string("CAN write: ") + std::strerror(write_error) : "short CAN frame write";
        close_socket();
        return publish(std::move(result));
    }

    // write 成功后不能因取消丢弃执行事实；只等待本事务 ACK 或固定截止时间，绝不重发。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool cancellation_observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        cancellation_observed = cancellation_observed || is_cancelled(cancelled);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                              std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        pollfd descriptor{fd_, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining > 50 ? 50 : remaining + 1));
        if (ready < 0) {
            if (errno == EINTR) continue;
            result.error = VehicleError::io_error;
            result.detail = system_error("CAN poll after write");
            break;
        }
        if (ready == 0) continue;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            result.error = VehicleError::io_error;
            result.detail = "CAN socket failed after write";
            break;
        }
        if ((descriptor.revents & POLLIN) == 0) continue;
        can_frame frame{};
        const auto bytes = ::read(fd_, &frame, sizeof(frame));
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            result.error = VehicleError::io_error;
            result.detail = system_error("CAN read after write");
            break;
        }
        if (bytes != static_cast<ssize_t>(sizeof(frame))) continue;
        const auto response = vehicle_can::decode_response(frame);
        if (!response || response->transaction != transaction) continue;
        cancellation_observed = cancellation_observed || is_cancelled(cancelled);
        if (response->status != vehicle_can::Status::ok) {
            result.error = response->status == vehicle_can::Status::invalid ?
                           VehicleError::invalid_response : VehicleError::rejected;
            result.detail = response->status == vehicle_can::Status::invalid ?
                            "ECU rejected invalid request" : "ECU rejected action";
            return publish(std::move(result));
        }
        if ((operation == vehicle_can::Operation::climate && response->climate_on != value) ||
            (operation == vehicle_can::Operation::left_front_window && response->left_front_window_open != value)) {
            result.error = VehicleError::invalid_response;
            result.detail = "ECU success receipt disagrees with requested state; action and state unknown";
            break;
        }
        result.climate_on = response->climate_on;
        result.left_front_window_open = response->left_front_window_open;
        result.action_applied = setter;
        needs_query_ = false;
        result.error = cancellation_observed ? VehicleError::cancelled : VehicleError::none;
        result.detail = cancellation_observed ? "cancelled after CAN write; ECU receipt retained" :
                                               "vcan0 ECU receipt confirmed";
        return publish(std::move(result));
    }
    // 失去 ACK 后既不能说执行了，也不能说没执行；后续 set 必须先由 query 恢复状态。
    needs_query_ = true;
    result.climate_on.reset();
    result.left_front_window_open.reset();
    result.action_applied.reset();
    if (result.error == VehicleError::none) {
        result.error = VehicleError::timeout;
        result.detail = "CAN receipt timeout after write; action and vehicle state unknown";
    }
    if (cancellation_observed) result.detail += "; cancellation observed while settling receipt";
    close_socket();
    return publish(std::move(result));
}

}  // namespace cabinflow::agent
