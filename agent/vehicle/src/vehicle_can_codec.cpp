#include <cabinflow/agent/vehicle_can_codec.hpp>

namespace cabinflow::agent::vehicle_can {
namespace {

can_frame make_frame(canid_t id, std::uint8_t code, std::uint32_t transaction, std::uint8_t value) noexcept {
    can_frame frame{};
    frame.can_id = id;
    frame.can_dlc = 8;
    frame.data[0] = version;
    frame.data[1] = code;
    // 事务号固定为网络字节序；不能依赖主机端序或结构体布局。
    frame.data[2] = static_cast<std::uint8_t>(transaction >> 24);
    frame.data[3] = static_cast<std::uint8_t>(transaction >> 16);
    frame.data[4] = static_cast<std::uint8_t>(transaction >> 8);
    frame.data[5] = static_cast<std::uint8_t>(transaction);
    frame.data[6] = value;
    return frame;
}

bool valid_envelope(const can_frame& frame, canid_t id) noexcept {
    return frame.can_id == id && frame.can_dlc == 8 && frame.data[0] == version && frame.data[7] == 0;
}

std::uint32_t transaction_of(const can_frame& frame) noexcept {
    return (static_cast<std::uint32_t>(frame.data[2]) << 24) |
           (static_cast<std::uint32_t>(frame.data[3]) << 16) |
           (static_cast<std::uint32_t>(frame.data[4]) << 8) |
           static_cast<std::uint32_t>(frame.data[5]);
}

}  // namespace

can_frame encode_request(const Request& request) noexcept {
    return make_frame(request_id, static_cast<std::uint8_t>(request.operation), request.transaction,
                      request.value ? 1 : 0);
}

can_frame encode_response(const Response& response) noexcept {
    const auto bits = static_cast<std::uint8_t>((response.climate_on ? 1 : 0) |
                                               (response.left_front_window_open ? 2 : 0));
    return make_frame(response_id, static_cast<std::uint8_t>(response.status), response.transaction, bits);
}

std::optional<Request> decode_request(const can_frame& frame) noexcept {
    if (!valid_envelope(frame, request_id) || frame.data[1] < 1 || frame.data[1] > 3 ||
        frame.data[6] > 1 || (frame.data[1] == 1 && frame.data[6] != 0)) return std::nullopt;
    return Request{static_cast<Operation>(frame.data[1]), transaction_of(frame), frame.data[6] == 1};
}

std::optional<Response> decode_response(const can_frame& frame) noexcept {
    if (!valid_envelope(frame, response_id) || frame.data[1] > 2 || frame.data[6] > 3) return std::nullopt;
    return Response{static_cast<Status>(frame.data[1]), transaction_of(frame),
                    (frame.data[6] & 1) != 0, (frame.data[6] & 2) != 0};
}

}  // namespace cabinflow::agent::vehicle_can
