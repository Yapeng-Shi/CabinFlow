#pragma once

#include <string_view>

namespace cabinflow::protocol {

enum class CodecError {
    kNone,
    kSerializationFailure,
    kMalformedMessage,
};

[[nodiscard]] inline std::string_view to_string(CodecError error) noexcept {
    switch (error) {
        case CodecError::kNone:
            return "none";
        case CodecError::kSerializationFailure:
            return "serialization_failure";
        case CodecError::kMalformedMessage:
            return "malformed_message";
    }

    return "unknown";
}

}  // namespace cabinflow::protocol
