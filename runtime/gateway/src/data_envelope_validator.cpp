#include <cabinflow/gateway/data_envelope_validator.hpp>

#include <utility>

namespace cabinflow::gateway {
namespace {

[[nodiscard]] bool has_response_identity(
    const protocol::MessageEnvelope& envelope) {
    return !envelope.message_id.empty() && !envelope.trace_id.empty() &&
           !envelope.session_id.empty() && !envelope.source_node.empty() &&
           !envelope.target_node.empty();
}

[[nodiscard]] bool has_required_data_fields(
    const protocol::MessageEnvelope& envelope) {
    return has_response_identity(envelope) && !envelope.work_id.empty() &&
           !envelope.topic.empty() && envelope.ttl_ms != 0U &&
           envelope.kind == protocol::MessageKind::kData;
}

[[nodiscard]] DataValidationResult make_error_result(
    const protocol::Message& message, DataValidationError error,
    bool should_close_connection) {
    DataValidationResult result;
    result.request.emplace(ValidatedDataMessage{message});
    result.error = error;
    result.can_return_error = true;
    result.should_close_connection = should_close_connection;
    return result;
}

}  // namespace

DataValidationResult DataEnvelopeValidator::validate(
    const protocol::Message& message) const {
    const auto& envelope = message.envelope;
    if (!has_response_identity(envelope)) {
        // 没有可靠的接收方和关联身份时，不发送可能误路由的错误响应。
        return {{}, DataValidationError::kInvalidEnvelope, false, true};
    }
    if (envelope.schema_version != protocol::kCurrentSchemaVersion) {
        return make_error_result(message, DataValidationError::kUnsupportedSchemaVersion,
                                 true);
    }
    if (!has_required_data_fields(envelope)) {
        return make_error_result(message, DataValidationError::kInvalidEnvelope,
                                 false);
    }

    DataValidationResult result;
    result.request.emplace(ValidatedDataMessage{message});
    return result;
}

}  // namespace cabinflow::gateway
