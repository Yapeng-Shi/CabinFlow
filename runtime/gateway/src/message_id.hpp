#pragma once

#include <string>
#include <string_view>

namespace cabinflow::gateway {

[[nodiscard]] std::string generate_gateway_message_id(std::string_view prefix);

}  // namespace cabinflow::gateway
