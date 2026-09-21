#include <cabinflow/gateway/control_envelope_validator.hpp>

#include <utility>

namespace cabinflow::gateway {
namespace {

[[nodiscard]] bool has_response_identity(
    const protocol::MessageEnvelope& envelope) {
    return !envelope.message_id.empty() && !envelope.trace_id.empty() &&
           !envelope.source_node.empty() && !envelope.target_node.empty();
}

[[nodiscard]] bool has_required_control_fields(
    const protocol::MessageEnvelope& envelope) {
    return has_response_identity(envelope) && envelope.ttl_ms != 0U &&
           envelope.topic == "control.request" &&
           envelope.kind == protocol::MessageKind::kData;
}

[[nodiscard]] ControlValidationResult make_error_result(
    const protocol::Message& message, protocol::v1::ControlRequest command,
    ControlValidationError error, bool can_return_error,
    bool should_close_connection) {
    ControlValidationResult result;
    result.request.emplace(ValidatedControlRequest{message, std::move(command)});
    result.error = error;
    result.can_return_error = can_return_error;
    result.should_close_connection = should_close_connection;
    return result;
}

}  // namespace

ControlValidationResult ControlEnvelopeValidator::validate(
    const protocol::Message& message) const {
    const auto& envelope = message.envelope;
    if (!has_response_identity(envelope)) {
        // 请求关联身份缺失时，不能安全构造关联错误，只能由连接层断开。
        return {{}, ControlValidationError::kInvalidEnvelope, false, true};
    }

    protocol::v1::ControlRequest command;
    if (!command.ParseFromString(message.payload)) {
        return {{}, ControlValidationError::kMalformedControlPayload, false, true};
    }
    if (envelope.schema_version != protocol::kCurrentSchemaVersion) {
        return make_error_result(message, std::move(command),
                                 ControlValidationError::kUnsupportedSchemaVersion,
                                 true, true);
    }
    if (!has_required_control_fields(envelope)) {
        return make_error_result(message, std::move(command),
                                 ControlValidationError::kInvalidEnvelope, true,
                                 false);
    }

    switch (command.command_case()) {
        case protocol::v1::ControlRequest::kRegisterUnit:
            if (!envelope.session_id.empty() || !envelope.work_id.empty()) {
                return make_error_result(message, std::move(command),
                                         ControlValidationError::kInvalidControlIdentity,
                                         true, false);
            }
            break;
        case protocol::v1::ControlRequest::kSetup:
            if (envelope.session_id.empty() || !envelope.work_id.empty()) {
                return make_error_result(message, std::move(command),
                                         ControlValidationError::kInvalidControlIdentity,
                                         true, false);
            }
            break;
        case protocol::v1::ControlRequest::kPause:
        case protocol::v1::ControlRequest::kExit:
            if (envelope.session_id.empty() || envelope.work_id.empty()) {
                return make_error_result(message, std::move(command),
                                         ControlValidationError::kInvalidControlIdentity,
                                         true, false);
            }
            break;
        case protocol::v1::ControlRequest::kTaskInfo:
            if (command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_WORK) {
                if (envelope.session_id.empty() || envelope.work_id.empty()) {
                    return make_error_result(message, std::move(command),
                                             ControlValidationError::kInvalidControlIdentity,
                                             true, false);
                }
            } else if (command.task_info().scope() ==
                       protocol::v1::TASK_INFO_SCOPE_SESSION) {
                if (envelope.session_id.empty() || !envelope.work_id.empty()) {
                    return make_error_result(message, std::move(command),
                                             ControlValidationError::kInvalidControlIdentity,
                                             true, false);
                }
            }
            break;
        case protocol::v1::ControlRequest::COMMAND_NOT_SET:
            break;
    }

    ControlValidationResult result;
    result.request.emplace(ValidatedControlRequest{message, std::move(command)});
    return result;
}

}  // namespace cabinflow::gateway
