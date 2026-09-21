#include "message_id.hpp"

#include <array>

#include <sys/random.h>

namespace cabinflow::gateway {

std::string generate_gateway_message_id(std::string_view prefix) {
    std::array<unsigned char, 16> random_bytes{};
    if (::getrandom(random_bytes.data(), random_bytes.size(), 0) !=
        static_cast<ssize_t>(random_bytes.size())) {
        return {};
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string message_id(prefix);
    message_id.reserve(message_id.size() + random_bytes.size() * 2U);
    for (const auto byte : random_bytes) {
        message_id.push_back(kHex[(byte >> 4U) & 0x0fU]);
        message_id.push_back(kHex[byte & 0x0fU]);
    }
    return message_id;
}

}  // namespace cabinflow::gateway
