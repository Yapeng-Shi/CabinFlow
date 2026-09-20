#include <cabinflow/protocol/message_codec.hpp>

#include <runtime_envelope.pb.h>

namespace cabinflow::protocol {
namespace {

v1::WireMessageKind to_wire_kind(MessageKind kind) {
    switch (kind) {
        case MessageKind::kData:
            return v1::WIRE_MESSAGE_KIND_DATA;
        case MessageKind::kCancel:
            return v1::WIRE_MESSAGE_KIND_CANCEL;
        case MessageKind::kError:
            return v1::WIRE_MESSAGE_KIND_ERROR;
    }

    return v1::WIRE_MESSAGE_KIND_UNSPECIFIED;
}

v1::WireSeatPosition to_wire_seat(SeatPosition seat) {
    switch (seat) {
        case SeatPosition::kUnknown:
            return v1::WIRE_SEAT_POSITION_UNKNOWN;
        case SeatPosition::kDriver:
            return v1::WIRE_SEAT_POSITION_DRIVER;
        case SeatPosition::kFrontPassenger:
            return v1::WIRE_SEAT_POSITION_FRONT_PASSENGER;
        case SeatPosition::kRearLeft:
            return v1::WIRE_SEAT_POSITION_REAR_LEFT;
        case SeatPosition::kRearCenter:
            return v1::WIRE_SEAT_POSITION_REAR_CENTER;
        case SeatPosition::kRearRight:
            return v1::WIRE_SEAT_POSITION_REAR_RIGHT;
    }

    return v1::WIRE_SEAT_POSITION_UNKNOWN;
}

bool from_wire_kind(v1::WireMessageKind wire_kind, MessageKind* kind) {
    switch (wire_kind) {
        case v1::WIRE_MESSAGE_KIND_DATA:
            *kind = MessageKind::kData;
            return true;
        case v1::WIRE_MESSAGE_KIND_CANCEL:
            *kind = MessageKind::kCancel;
            return true;
        case v1::WIRE_MESSAGE_KIND_ERROR:
            *kind = MessageKind::kError;
            return true;
        case v1::WIRE_MESSAGE_KIND_UNSPECIFIED:
            return false;
        default:
            return false;
    }
}

bool from_wire_seat(v1::WireSeatPosition wire_seat, SeatPosition* seat) {
    switch (wire_seat) {
        case v1::WIRE_SEAT_POSITION_UNKNOWN:
            *seat = SeatPosition::kUnknown;
            return true;
        case v1::WIRE_SEAT_POSITION_DRIVER:
            *seat = SeatPosition::kDriver;
            return true;
        case v1::WIRE_SEAT_POSITION_FRONT_PASSENGER:
            *seat = SeatPosition::kFrontPassenger;
            return true;
        case v1::WIRE_SEAT_POSITION_REAR_LEFT:
            *seat = SeatPosition::kRearLeft;
            return true;
        case v1::WIRE_SEAT_POSITION_REAR_CENTER:
            *seat = SeatPosition::kRearCenter;
            return true;
        case v1::WIRE_SEAT_POSITION_REAR_RIGHT:
            *seat = SeatPosition::kRearRight;
            return true;
        default:
            return false;
    }
}

}  // namespace

EncodeResult encode_message(const Message& message) {
    v1::RuntimeMessage wire_message;
    const auto& envelope = message.envelope;
    wire_message.set_schema_version(envelope.schema_version);
    wire_message.set_message_id(envelope.message_id);
    wire_message.set_trace_id(envelope.trace_id);
    wire_message.set_session_id(envelope.session_id);
    wire_message.set_work_id(envelope.work_id);
    wire_message.set_source_node(envelope.source_node);
    wire_message.set_target_node(envelope.target_node);
    wire_message.set_topic(envelope.topic);
    wire_message.set_kind(to_wire_kind(envelope.kind));
    wire_message.set_seat(to_wire_seat(envelope.seat));
    wire_message.set_sequence(envelope.sequence);
    wire_message.set_created_monotonic_ns(envelope.created_monotonic_ns);
    wire_message.set_ttl_ms(envelope.ttl_ms);
    wire_message.set_is_final(envelope.is_final);
    wire_message.set_payload(message.payload);

    EncodeResult result;
    if (!wire_message.SerializeToString(&result.bytes)) {
        result.error = CodecError::kSerializationFailure;
    }
    return result;
}

DecodeResult decode_message(const std::string& bytes) {
    v1::RuntimeMessage wire_message;
    if (!wire_message.ParseFromString(bytes)) {
        return {{}, CodecError::kMalformedMessage};
    }

    DecodeResult result;
    auto& envelope = result.message.envelope;
    if (!from_wire_kind(wire_message.kind(), &envelope.kind) ||
        !from_wire_seat(wire_message.seat(), &envelope.seat)) {
        result.error = CodecError::kMalformedMessage;
        return result;
    }

    envelope.schema_version =
        static_cast<std::uint16_t>(wire_message.schema_version());
    envelope.message_id = wire_message.message_id();
    envelope.trace_id = wire_message.trace_id();
    envelope.session_id = wire_message.session_id();
    envelope.work_id = wire_message.work_id();
    envelope.source_node = wire_message.source_node();
    envelope.target_node = wire_message.target_node();
    envelope.topic = wire_message.topic();
    envelope.sequence = wire_message.sequence();
    envelope.created_monotonic_ns = wire_message.created_monotonic_ns();
    envelope.ttl_ms = wire_message.ttl_ms();
    envelope.is_final = wire_message.is_final();
    result.message.payload = wire_message.payload();
    return result;
}

}  // namespace cabinflow::protocol
