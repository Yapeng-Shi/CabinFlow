#pragma once

#include <cstdint>
#include <linux/can.h>
#include <optional>

namespace cabinflow::agent::vehicle_can {

constexpr canid_t request_id = 0x600;
constexpr canid_t response_id = 0x601;
constexpr std::uint8_t version = 1;

enum class Operation : std::uint8_t { query = 1, climate = 2, left_front_window = 3 };
enum class Status : std::uint8_t { ok = 0, invalid = 1, rejected = 2 };

struct Request {
    Operation operation;
    std::uint32_t transaction;
    bool value;
};

struct Response {
    Status status;
    std::uint32_t transaction;
    bool climate_on;
    bool left_front_window_open;
};

[[nodiscard]] can_frame encode_request(const Request& request) noexcept;
[[nodiscard]] can_frame encode_response(const Response& response) noexcept;
[[nodiscard]] std::optional<Request> decode_request(const can_frame& frame) noexcept;
[[nodiscard]] std::optional<Response> decode_response(const can_frame& frame) noexcept;

}  // namespace cabinflow::agent::vehicle_can
