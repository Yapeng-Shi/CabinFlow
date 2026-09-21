#pragma once

#include <optional>

#include <cabinflow/protocol/message.hpp>

namespace cabinflow::gateway {

enum class DataValidationError {
    kNone,
    kUnsupportedSchemaVersion,
    kInvalidEnvelope,
};

struct ValidatedDataMessage {
    protocol::Message message;
};

struct DataValidationResult {
    std::optional<ValidatedDataMessage> request;
    DataValidationError error{DataValidationError::kNone};
    bool can_return_error{false};
    bool should_close_connection{false};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == DataValidationError::kNone;
    }
};

// 数据面只校验通用 Envelope；payload 的唯一类型由 TargetNode 的 topic 决定。
class DataEnvelopeValidator final {
public:
    [[nodiscard]] DataValidationResult validate(
        const protocol::Message& message) const;
};

}  // namespace cabinflow::gateway
