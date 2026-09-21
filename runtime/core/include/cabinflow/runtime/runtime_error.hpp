#pragma once

#include <string_view>

namespace cabinflow::runtime {

enum class RuntimeError {
    kNone,
    kInvalidNode,
    kTargetNodeRequiresTargetRegistration,
    kInvalidTargetQueueCapacity,
    kEmptyTargetNodeName,
    kDuplicateTargetNodeName,
    kAlreadyStarted,
    kNodeStartFailure,
    kTargetWorkerStartFailure,
};

[[nodiscard]] std::string_view to_string(RuntimeError error) noexcept;

}  // namespace cabinflow::runtime
