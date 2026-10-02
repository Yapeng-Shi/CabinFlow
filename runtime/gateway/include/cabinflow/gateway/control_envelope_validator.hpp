#pragma once

#include <optional>

#include <control.pb.h>

#include <cabinflow/protocol/message.hpp>

namespace cabinflow::runtime {
class Clock;
}

namespace cabinflow::gateway {

enum class ControlValidationError {
    kNone,
    kMalformedControlPayload,
    kUnsupportedSchemaVersion,
    kInvalidEnvelope,
    kInvalidControlIdentity,
    kExpired,
};

struct ValidatedControlRequest {
    protocol::Message message;
    protocol::v1::ControlRequest command;
};

struct ControlValidationResult {
    std::optional<ValidatedControlRequest> request;
    ControlValidationError error{ControlValidationError::kNone};
    bool can_return_error{false};
    bool should_close_connection{false};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ControlValidationError::kNone;
    }
};

// 只做控制面准入与唯一类型解码；不会把 setup 送入 SessionLedger。
class ControlEnvelopeValidator final {
public:
    explicit ControlEnvelopeValidator(const runtime::Clock& clock) : clock_(clock) {}

    [[nodiscard]] ControlValidationResult validate(
        const protocol::Message& message) const;

private:
    const runtime::Clock& clock_;
};

}  // namespace cabinflow::gateway
