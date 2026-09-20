#include <iostream>
#include <string_view>

#include <cabinflow/protocol/message_codec.hpp>

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

}  // namespace

int main() {
    using cabinflow::protocol::CodecError;
    using cabinflow::protocol::Message;
    using cabinflow::protocol::MessageKind;
    using cabinflow::protocol::SeatPosition;

    Message original;
    original.envelope.schema_version = 1;
    original.envelope.message_id = "message-42";
    original.envelope.trace_id = "trace-42";
    original.envelope.session_id = "driver-session";
    original.envelope.work_id = "route-work";
    original.envelope.source_node = "text-source";
    original.envelope.target_node = "echo-processor";
    original.envelope.topic = "demo.text";
    original.envelope.kind = MessageKind::kCancel;
    original.envelope.seat = SeatPosition::kDriver;
    original.envelope.sequence = 7;
    original.envelope.created_monotonic_ns = 123456789;
    original.envelope.ttl_ms = 500;
    original.envelope.is_final = true;
    original.payload = "cancel navigation";

    const auto encoded = cabinflow::protocol::encode_message(original);
    if (!expect(static_cast<bool>(encoded), "encode succeeds") ||
        !expect(!encoded.bytes.empty(), "encoded bytes")) {
        return 1;
    }

    const auto decoded = cabinflow::protocol::decode_message(encoded.bytes);
    const auto& envelope = decoded.message.envelope;
    if (!expect(static_cast<bool>(decoded), "decode succeeds") ||
        !expect(envelope.message_id == original.envelope.message_id,
                "message id") ||
        !expect(envelope.trace_id == original.envelope.trace_id, "trace id") ||
        !expect(envelope.session_id == original.envelope.session_id,
                "session id") ||
        !expect(envelope.work_id == original.envelope.work_id, "work id") ||
        !expect(envelope.kind == MessageKind::kCancel, "message kind") ||
        !expect(envelope.seat == SeatPosition::kDriver, "seat") ||
        !expect(envelope.sequence == 7, "sequence") ||
        !expect(envelope.ttl_ms == 500, "ttl") ||
        !expect(envelope.is_final, "final") ||
        !expect(decoded.message.payload == original.payload, "payload")) {
        return 1;
    }

    const auto malformed = cabinflow::protocol::decode_message("not protobuf");
    if (!expect(malformed.error == CodecError::kMalformedMessage,
                "malformed input")) {
        return 1;
    }

    return 0;
}
