#pragma once

#include <string_view>

namespace cabinflow::runtime {

enum class RuntimeError {
    kNone,
    kInvalidNode,
    kAlreadyStarted,
    kNodeStartFailure,
};

[[nodiscard]] std::string_view to_string(RuntimeError error) noexcept;

}  // namespace cabinflow::runtime
