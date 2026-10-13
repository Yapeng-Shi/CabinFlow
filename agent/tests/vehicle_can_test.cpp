#include <cabinflow/agent/fake_vehicle.hpp>
#include <cabinflow/agent/socket_can_vehicle.hpp>
#include <cabinflow/agent/vehicle_can_codec.hpp>
#include <cabinflow/agent/vcan_interface.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <future>
#include <iostream>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace cabinflow::agent;
namespace can = cabinflow::agent::vehicle_can;
using namespace std::chrono_literals;

void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }

void codec_test() {
    const auto request = can::encode_request({can::Operation::climate, 0x12345678, true});
    require(request.can_id == 0x600 && request.can_dlc == 8, "classical 11-bit CAN request envelope");
    const unsigned char expected[]{1, 2, 0x12, 0x34, 0x56, 0x78, 1, 0};
    require(std::memcmp(request.data, expected, 8) == 0, "exact request bytes and transaction big-endian");
    const auto decoded = can::decode_request(request);
    require(decoded && decoded->transaction == 0x12345678 && decoded->value &&
            decoded->operation == can::Operation::climate, "request round trip");
    for (const auto operation : {can::Operation::query, can::Operation::climate, can::Operation::left_front_window}) {
        const auto frame = can::encode_request({operation, 0xffffffff, false});
        require(can::decode_request(frame).has_value(), "all supported operations decode");
    }
    for (const auto status : {can::Status::ok, can::Status::invalid, can::Status::rejected}) {
        for (unsigned bits = 0; bits < 4; ++bits) {
            const auto response = can::encode_response({status, 0x12345678, (bits & 1) != 0, (bits & 2) != 0});
            const auto result = can::decode_response(response);
            require(result && result->transaction == 0x12345678 && result->status == status &&
                    result->climate_on == ((bits & 1) != 0) &&
                    result->left_front_window_open == ((bits & 2) != 0), "typed response and state bits");
        }
    }
    const auto response = can::encode_response({can::Status::ok, 9, false, false});
    for (const auto flag : {CAN_EFF_FLAG, CAN_RTR_FLAG, CAN_ERR_FLAG}) {
        auto invalid = response;
        invalid.can_id |= flag;
        require(!can::decode_response(invalid), "reject EFF, RTR and error frames");
        invalid = request;
        invalid.can_id |= flag;
        require(!can::decode_request(invalid), "request rejects non-classical-data frames");
    }
    for (unsigned fault = 0; fault < 6; ++fault) {
        auto invalid = response;
        if (fault == 0) invalid.can_id = 0x602;
        if (fault == 1) invalid.can_dlc = 7;
        if (fault == 2) invalid.data[0] = 2;
        if (fault == 3) invalid.data[7] = 1;
        if (fault == 4) invalid.data[1] = 3;
        if (fault == 5) invalid.data[6] = 4;
        require(!can::decode_response(invalid), "reject wrong ID/DLC/version/reserved/status/state bits");
    }
    auto invalid = can::encode_request({can::Operation::query, 1, true});
    require(!can::decode_request(invalid), "query value must be zero");
    invalid = request;
    invalid.data[1] = 4;
    require(!can::decode_request(invalid), "unsupported opcode rejected");
    invalid = request;
    invalid.data[6] = 2;
    require(!can::decode_request(invalid), "setter must carry binary value");
}

void unit_test() {
    codec_test();
    std::string kind_error;
    const auto loopback = ::if_nametoindex("lo");
    require(loopback != 0 && !can::is_vcan_interface(loopback, kind_error) && !kind_error.empty(),
            "kernel interface kind check rejects non-vcan even when interface exists");
    FakeVehicle fake(false, false);
    require(fake.query().climate_on == false && fake.snapshot().left_front_window_open == false,
            "fake explicit initial state");
    const SubmissionGate deny = [](const std::function<void()>&) { return false; };
    auto cancelled = fake.set_climate(true, {}, deny);
    require(cancelled.error == VehicleError::cancelled && cancelled.action_applied == false &&
            cancelled.climate_on == false, "submission gate refusal causes zero fake action");
    std::atomic<bool> cancel{false};
    const SubmissionGate submit_then_cancel = [&](const std::function<void()>& action) {
        action();
        cancel = true;
        return true;
    };
    auto applied = fake.set_left_front_window(true, [&] { return cancel.load(); }, submit_then_cancel);
    require(applied.error == VehicleError::cancelled && applied.action_applied == true && applied.left_front_window_open == true,
            "action fact retained when cancel arrives after submission");
    require(fake.set_left_front_window(false, [] { return true; }).action_applied == false &&
            fake.snapshot().left_front_window_open == true, "predicate cancels without action");

    SocketCanVehicle backend;
    cancelled = backend.query([] { return true; });
    require(cancelled.error == VehicleError::cancelled && cancelled.action_applied == false &&
            !cancelled.climate_on && !cancelled.left_front_window_open,
            "CAN pre-write cancellation leaves initial unknown state and no action");
    require(backend.set_climate(true).error == VehicleError::state_unknown,
            "initial setter refuses until explicit query");
    if (::if_nametoindex("vcan0") == 0) {
        const auto unavailable = backend.query();
        require(unavailable.error == VehicleError::unavailable && !unavailable.detail.empty() &&
                unavailable.action_applied == false && !unavailable.climate_on,
                "unavailable vcan0 explicit failure; no fake state");
        std::cout << "unavailable-interface path verified; live vcan integration pending\n";
    } else {
        std::cout << "vcan0 exists; live bus checks require --integration <vehicle_ecu>\n";
    }
    std::cout << "vehicle_can_unit_test passed\n";
}

class PeerSocket {
public:
    PeerSocket() {
        fd_ = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
        require(fd_ >= 0, "test peer CAN socket");
        try {
            const auto index = ::if_nametoindex("vcan0");
            require(index != 0, "vcan0 unavailable: integration pending, not a passing test");
            const can_filter filter{can::request_id, CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG};
            require(::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) == 0, "peer filter");
            sockaddr_can address{};
            address.can_family = AF_CAN;
            address.can_ifindex = static_cast<int>(index);
            require(::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "peer bind");
        } catch (...) {
            ::close(fd_);
            throw;
        }
    }
    ~PeerSocket() { ::close(fd_); }
    can::Request receive() {
        const auto deadline = std::chrono::steady_clock::now() + 2500ms;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{fd_, POLLIN, 0};
            require(::poll(&descriptor, 1, 50) >= 0, "peer poll");
            if ((descriptor.revents & POLLIN) == 0) continue;
            can_frame frame{};
            require(::read(fd_, &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)), "peer read frame");
            const auto request = can::decode_request(frame);
            if (request) return *request;
        }
        throw std::runtime_error("peer request timeout");
    }
    void send(const can_frame& frame) {
        require(::write(fd_, &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame)), "peer response write");
    }
private:
    int fd_ = -1;
};

class OwnedEcu {
public:
    explicit OwnedEcu(const char* executable) {
        int output[2];
        require(::pipe(output) == 0, "ECU readiness pipe");
        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(output[0]);
            ::close(output[1]);
            throw std::runtime_error("ECU fork");
        }
        if (pid_ == 0) {
            ::close(output[0]);
            if (::dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
            ::close(output[1]);
            ::execl(executable, executable, static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(output[1]);
        output_ = output[0];
        pollfd descriptor{output_, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 2500);
        char line[512]{};
        const auto bytes = ready > 0 ? ::read(output_, line, sizeof(line) - 1) : -1;
        if (bytes <= 0 || std::string(line).find("vehicle_ecu ready") == std::string::npos) {
            stop();
            throw std::runtime_error("ECU did not become ready");
        }
    }
    ~OwnedEcu() { stop(); }
    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
            exit_status_ = status;
        }
        if (output_ >= 0) {
            ::close(output_);
            output_ = -1;
        }
    }
    bool graceful() const { return WIFEXITED(exit_status_) && WEXITSTATUS(exit_status_) == 0; }
private:
    pid_t pid_ = -1;
    int output_ = -1;
    int exit_status_ = -1;
};

template <typename F>
VehicleSnapshot exchange(PeerSocket& peer, SocketCanVehicle& backend, F respond) {
    auto operation = std::async(std::launch::async, [&] { return backend.query(); });
    respond(peer.receive());
    return operation.get();
}

void integration_test(const char* ecu) {
    require(::if_nametoindex("vcan0") != 0, "vcan0 unavailable: CAN integration pending (no network setup performed)");
    {
        OwnedEcu process(ecu);
        SocketCanVehicle backend;
        auto state = backend.query();
        require(state.error == VehicleError::none && state.climate_on == false &&
                state.left_front_window_open == false && state.action_applied == false,
                ("independent ECU initial state query: " + state.detail).c_str());
        state = backend.set_climate(true);
        require(state.error == VehicleError::none && state.action_applied == true && state.climate_on == true,
                "independent ECU climate action receipt");
        state = backend.set_left_front_window(true);
        require(state.error == VehicleError::none && state.action_applied == true && state.left_front_window_open == true,
                "independent ECU window action receipt");
        require(backend.query().climate_on == true && backend.snapshot().left_front_window_open == true,
                "query confirms independent ECU changes");
        require(backend.set_climate(false, {}, [](const std::function<void()>&) { return false; }).action_applied == false,
                "CAN gate refusal writes no setter");
        require(backend.query().climate_on == true, "ECU state unchanged by denied setter");
        process.stop();
        require(process.graceful(), "owned ECU exits normally on SIGTERM");
        state = backend.query();
        require(state.error == VehicleError::timeout && !state.climate_on && !state.left_front_window_open &&
                !state.action_applied && backend.set_climate(false).error == VehicleError::state_unknown,
                "ECU disappearance loses receipt, clears known state and blocks setters until explicit query");
    }

    PeerSocket peer;
    SocketCanVehicle backend;
    auto state = exchange(peer, backend, [&](const can::Request& request) {
        auto stale = can::encode_response({can::Status::ok, request.transaction + 1, true, true});
        peer.send(stale);
        auto wrong = can::encode_response({can::Status::ok, request.transaction, true, true});
        wrong.data[0] = 2;
        peer.send(wrong);
        peer.send(can::encode_response({can::Status::ok, request.transaction, false, false}));
    });
    require(state.error == VehicleError::none && state.climate_on == false,
            "stale transaction and malformed response cannot supply receipt state");

    auto filtered = std::async(std::launch::async, [&] { return backend.query(); });
    const auto filtered_request = peer.receive();
    for (const auto flag : {CAN_EFF_FLAG, CAN_RTR_FLAG}) {
        auto invalid = can::encode_response({can::Status::ok, filtered_request.transaction, true, true});
        invalid.can_id |= flag;
        peer.send(invalid);
    }
    auto wrong_id = can::encode_response({can::Status::ok, filtered_request.transaction, true, true});
    wrong_id.can_id = 0x602;
    peer.send(wrong_id);
    // 同事务号的非法帧也不能提前结束等待或污染状态，不能只检查最终正确 ACK 的结果。
    require(filtered.wait_for(100ms) == std::future_status::timeout,
            "EFF/RTR/wrong-ID responses cannot complete pending query");
    const auto unchanged = backend.snapshot();
    require(unchanged.climate_on == false && unchanged.left_front_window_open == false,
            "invalid responses leave previously confirmed state unchanged");
    peer.send(can::encode_response({can::Status::ok, filtered_request.transaction, false, false}));
    state = filtered.get();
    require(state.error == VehicleError::none && state.climate_on == false && state.left_front_window_open == false,
            "matching classical data response completes pending query");

    auto rejected = std::async(std::launch::async, [&] { return backend.set_climate(true); });
    const auto rejected_request = peer.receive();
    peer.send(can::encode_response({can::Status::rejected, rejected_request.transaction, true, true}));
    state = rejected.get();
    require(state.error == VehicleError::rejected && state.action_applied == false && state.climate_on == false,
            "rejected setter preserves confirmed state and reports no action");

    std::atomic<bool> cancelled{false};
    auto setter = std::async(std::launch::async, [&] {
        return backend.set_climate(true, [&] { return cancelled.load(); });
    });
    const auto applied_request = peer.receive();
    cancelled = true;
    auto readable = std::async(std::launch::async, [&] { return backend.snapshot(); });
    require(readable.wait_for(100ms) == std::future_status::ready, "ACK wait leaves backend snapshot lock free");
    peer.send(can::encode_response({can::Status::ok, applied_request.transaction, true, false}));
    state = setter.get();
    require(state.error == VehicleError::cancelled && state.action_applied == true && state.climate_on == true,
            "cancel after write settles ACK and retains action fact");

    const auto start = std::chrono::steady_clock::now();
    auto timeout = std::async(std::launch::async, [&] { return backend.set_left_front_window(true); });
    const auto lost_request = peer.receive();
    (void)lost_request;
    state = timeout.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(state.error == VehicleError::timeout && !state.action_applied && !state.climate_on &&
            !state.left_front_window_open && elapsed >= 1900ms && elapsed < 3000ms,
            "lost ACK reaches 2s deadline and leaves action and state unknown");
    require(backend.set_climate(false).error == VehicleError::state_unknown,
            "setter refuses after uncertain write");
    state = exchange(peer, backend, [&](const can::Request& request) {
        peer.send(can::encode_response({can::Status::ok, request.transaction, true, true}));
    });
    require(state.error == VehicleError::none && state.action_applied == false && state.left_front_window_open == true,
            "explicit successful query restores state after timeout");
    auto mismatch = std::async(std::launch::async, [&] { return backend.set_climate(false); });
    const auto mismatch_request = peer.receive();
    peer.send(can::encode_response({can::Status::ok, mismatch_request.transaction, true, true}));
    state = mismatch.get();
    require(state.error == VehicleError::invalid_response && !state.action_applied && !state.climate_on &&
            !state.left_front_window_open && backend.set_climate(false).error == VehicleError::state_unknown,
            "OK status with wrong requested bit cannot confirm action or state; requires explicit query");
    std::cout << "vehicle_can_integration passed (vcan simulation only)\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--unit") unit_test();
        else if (argc == 3 && std::string(argv[1]) == "--integration") integration_test(argv[2]);
        else throw std::runtime_error("usage: vehicle_can_test --unit | --integration <vehicle_ecu-path>");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
