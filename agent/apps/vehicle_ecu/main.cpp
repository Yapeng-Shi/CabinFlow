#include <cabinflow/agent/vehicle_can_codec.hpp>
#include <cabinflow/agent/vcan_interface.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }

struct OwnedSocket {
    int fd = -1;
    ~OwnedSocket() { if (fd >= 0) ::close(fd); }
};

int fail(const char* operation) {
    std::cerr << operation << ": " << std::strerror(errno) << '\n';
    return 1;
}
}  // namespace

int main() {
    using namespace cabinflow::agent::vehicle_can;
    struct sigaction action{};
    action.sa_handler = request_stop;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGTERM, &action, nullptr) != 0 || ::sigaction(SIGINT, &action, nullptr) != 0)
        return fail("ECU signal handler");

    OwnedSocket socket;
    socket.fd = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (socket.fd < 0) return fail("ECU SocketCAN socket");
    ifreq interface{};
    std::strncpy(interface.ifr_name, "vcan0", IFNAMSIZ - 1);
    if (::ioctl(socket.fd, SIOCGIFINDEX, &interface) != 0) return fail("ECU vcan0 interface");
    std::string kind_error;
    if (!is_vcan_interface(static_cast<unsigned>(interface.ifr_ifindex), kind_error)) {
        std::cerr << kind_error << '\n';
        return 1;
    }
    // 只订阅精确 SFF 数据 ID；CAN_ERR_FLAG 放进 mask 会误选内核错误帧接收列表。
    const can_filter filter{request_id, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
    if (::setsockopt(socket.fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) != 0)
        return fail("ECU request filter");
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = interface.ifr_ifindex;
    if (::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return fail("ECU vcan0 bind");

    // 本进程唯一拥有模拟 ECU 状态，启动值显式为关/关；后端不能自行假定它。
    bool climate_on = false;
    bool left_front_window_open = false;
    std::cout << "vehicle_ecu ready interface=vcan0 simulated=true climate_on=0 left_front_window_open=0" << std::endl;
    while (!stopping) {
        pollfd descriptor{socket.fd, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return fail("ECU poll");
        }
        if (ready == 0) continue;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cerr << "ECU CAN socket unavailable\n";
            return 1;
        }
        can_frame frame{};
        const auto bytes = ::read(socket.fd, &frame, sizeof(frame));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return fail("ECU read");
        }
        if (bytes != static_cast<ssize_t>(sizeof(frame))) continue;
        const auto request = decode_request(frame);
        if (!request) continue;
        if (request->operation == Operation::climate) climate_on = request->value;
        if (request->operation == Operation::left_front_window) left_front_window_open = request->value;
        const auto response = encode_response({Status::ok, request->transaction, climate_on, left_front_window_open});
        if (::write(socket.fd, &response, sizeof(response)) != static_cast<ssize_t>(sizeof(response)))
            return fail("ECU response write");
        std::cout << "txn=" << request->transaction << " climate_on=" << climate_on
                  << " left_front_window_open=" << left_front_window_open << std::endl;
    }
    std::cout << "vehicle_ecu stopped\n";
    return 0;
}
