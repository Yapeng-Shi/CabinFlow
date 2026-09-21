#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <delivery.pb.h>

#include <cabinflow/gateway/data_envelope_validator.hpp>
#include <cabinflow/gateway/delivery_error_builder.hpp>
#include <cabinflow/gateway/response_sequencer.hpp>

#include "fake_clock.hpp"

namespace {

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

cabinflow::protocol::Message make_message() {
    cabinflow::protocol::Message message;
    message.envelope.message_id = "request-1";
    message.envelope.trace_id = "trace-1";
    message.envelope.session_id = "session-1";
    message.envelope.work_id = "work-1";
    message.envelope.source_node = "tcp-client";
    message.envelope.target_node = "dialogue.primary";
    message.envelope.topic = "cockpit.text.input";
    message.envelope.sequence = 4;
    message.envelope.created_monotonic_ns = 12;
    message.envelope.ttl_ms = 100;
    return message;
}

void test_data_envelope_contract() {
    cabinflow::gateway::DataEnvelopeValidator validator;
    const auto valid = validator.validate(make_message());
    require(static_cast<bool>(valid), "valid data envelope is admitted for routing");

    auto missing_work = make_message();
    missing_work.envelope.work_id.clear();
    const auto missing_work_result = validator.validate(missing_work);
    require(missing_work_result.can_return_error &&
                missing_work_result.error ==
                    cabinflow::gateway::DataValidationError::kInvalidEnvelope,
            "data messages require a non-empty work identity");

    auto unsupported_schema = make_message();
    unsupported_schema.envelope.schema_version = 2;
    const auto schema_result = validator.validate(unsupported_schema);
    require(schema_result.can_return_error && schema_result.should_close_connection &&
                schema_result.error ==
                    cabinflow::gateway::DataValidationError::kUnsupportedSchemaVersion,
            "unsupported schema returns a typed error then closes");

    auto missing_source = make_message();
    missing_source.envelope.source_node.clear();
    const auto unsafe_result = validator.validate(missing_source);
    require(!unsafe_result.can_return_error && unsafe_result.should_close_connection,
            "unroutable request identity closes without a guessed response");
}

void test_delivery_error_identity_and_sequence() {
    cabinflow::test::FakeClock clock(500);
    cabinflow::gateway::ResponseSequencer sequencer;
    cabinflow::gateway::DeliveryErrorBuilder builder(clock, sequencer, 250);
    const auto request = make_message();
    const auto first = builder.build(
        request, cabinflow::protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET,
        "unknown target node", "connection-1");
    require(static_cast<bool>(first), "delivery error can be built");
    require(first.message.envelope.message_id != request.envelope.message_id &&
                first.message.envelope.trace_id == request.envelope.trace_id &&
                first.message.envelope.session_id == request.envelope.session_id &&
                first.message.envelope.work_id == request.envelope.work_id &&
                first.message.envelope.source_node == request.envelope.target_node &&
                first.message.envelope.target_node == request.envelope.source_node &&
                first.message.envelope.topic == "runtime.delivery.error" &&
                first.message.envelope.kind == cabinflow::protocol::MessageKind::kError &&
                first.message.envelope.sequence == 0 &&
                first.message.envelope.created_monotonic_ns == 500 &&
                first.message.envelope.ttl_ms == 250 && first.message.envelope.is_final,
            "delivery error has independent response identity and stream metadata");

    cabinflow::protocol::v1::DeliveryError decoded;
    require(decoded.ParseFromString(first.message.payload) &&
                decoded.request_message_id() == request.envelope.message_id &&
                decoded.code() ==
                    cabinflow::protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET,
            "delivery error payload carries typed rejection and request correlation");

    const auto second = builder.build(
        request, cabinflow::protocol::v1::DELIVERY_ERROR_QUEUE_FULL,
        "target queue is full", "connection-1");
    require(static_cast<bool>(second) && second.message.envelope.sequence == 1 &&
                second.message.envelope.message_id != first.message.envelope.message_id,
            "each emitted error has a unique message id and response sequence");
}

}  // namespace

int main() {
    try {
        test_data_envelope_contract();
        test_delivery_error_identity_and_sequence();
        std::cout << "delivery error contract test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "delivery error contract test failed: " << error.what() << '\n';
        return 1;
    }
}
