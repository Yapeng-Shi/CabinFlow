#pragma once

#include <string_view>

namespace cabinflow::transport {

enum class TransportError {
    kNone,
    kInvalidTopic,
    kEmptyHandler,
    kUnavailable,
    kCodecFailure,
    kIoFailure,
    kClosed,
};

[[nodiscard]] inline std::string_view to_string(TransportError error) noexcept {
    switch (error) {
        case TransportError::kNone:
            return "none";
        case TransportError::kInvalidTopic:
            return "invalid_topic";
        case TransportError::kEmptyHandler:
            return "empty_handler";
        case TransportError::kUnavailable:
            return "unavailable";
        case TransportError::kCodecFailure:
            return "codec_failure";
        case TransportError::kIoFailure:
            return "io_failure";
        case TransportError::kClosed:
            return "closed";
    }

    return "unknown";
}

}  // namespace cabinflow::transport
