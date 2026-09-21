#include <cabinflow/gateway/delivery_error_builder.hpp>

#include "message_id.hpp"

namespace cabinflow::gateway {

DeliveryErrorBuilder::DeliveryErrorBuilder(const runtime::Clock& clock,
                                           ResponseSequencer& sequencer,
                                           std::uint32_t response_ttl_ms)
    : clock_(clock), sequencer_(sequencer), response_ttl_ms_(response_ttl_ms) {}

DeliveryErrorBuildResult DeliveryErrorBuilder::build(
    const protocol::Message& request, protocol::v1::DeliveryErrorCode code,
    std::string_view detail, std::string_view connection_id) const {
    if (response_ttl_ms_ == 0U) {
        return {{}, DeliveryErrorBuildError::kInvalidResponseTtl};
    }

    const auto sequence = sequencer_.next(request.envelope.session_id,
                                          request.envelope.work_id,
                                          "runtime.delivery.error", connection_id);
    if (!sequence) {
        return {{}, DeliveryErrorBuildError::kSequenceFailure};
    }
    const auto message_id = generate_gateway_message_id("delivery-error-");
    if (message_id.empty()) {
        return {{}, DeliveryErrorBuildError::kMessageIdGenerationFailure};
    }

    protocol::v1::DeliveryError error;
    error.set_request_message_id(request.envelope.message_id);
    error.set_code(code);
    error.set_message(std::string(detail));

    protocol::Message message;
    auto& envelope = message.envelope;
    envelope.schema_version = protocol::kCurrentSchemaVersion;
    envelope.message_id = message_id;
    envelope.trace_id = request.envelope.trace_id;
    envelope.session_id = request.envelope.session_id;
    envelope.work_id = request.envelope.work_id;
    envelope.source_node = request.envelope.target_node;
    envelope.target_node = request.envelope.source_node;
    envelope.topic = "runtime.delivery.error";
    envelope.kind = protocol::MessageKind::kError;
    envelope.sequence = sequence.sequence;
    envelope.created_monotonic_ns = clock_.now_monotonic_ns();
    envelope.ttl_ms = response_ttl_ms_;
    envelope.is_final = true;

    if (!error.SerializeToString(&message.payload)) {
        return {{}, DeliveryErrorBuildError::kSerializationFailure};
    }
    return {std::move(message), DeliveryErrorBuildError::kNone};
}

}  // namespace cabinflow::gateway
