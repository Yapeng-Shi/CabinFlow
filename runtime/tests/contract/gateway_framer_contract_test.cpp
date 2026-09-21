#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <cabinflow/gateway/runtime_message_framer.hpp>

namespace {

using cabinflow::gateway::FrameError;
using cabinflow::gateway::RuntimeMessageFramer;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

Message make_message(std::string message_id, std::string payload) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace";
    message.envelope.session_id = "session";
    message.envelope.work_id = "work";
    message.envelope.source_node = "gateway-client";
    message.envelope.target_node = "runtime";
    message.envelope.topic = "control.request";
    message.envelope.kind = MessageKind::kData;
    message.envelope.sequence = 1;
    message.envelope.created_monotonic_ns = 1;
    message.envelope.ttl_ms = 1000;
    message.payload = std::move(payload);
    return message;
}

void append_u32_be(std::string* bytes, std::uint32_t value) {
    bytes->push_back(static_cast<char>((value >> 24U) & 0xFFU));
    bytes->push_back(static_cast<char>((value >> 16U) & 0xFFU));
    bytes->push_back(static_cast<char>((value >> 8U) & 0xFFU));
    bytes->push_back(static_cast<char>(value & 0xFFU));
}

void test_encoder_writes_big_endian_length_prefix() {
    const auto encoded = RuntimeMessageFramer::encode(make_message("message-1", "payload"));
    require(static_cast<bool>(encoded), "encode frame for prefix check");
    require(encoded.bytes.size() > cabinflow::gateway::kFrameHeaderBytes,
            "frame must contain header and protobuf body");

    const auto header_size =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded.bytes[0])) << 24U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded.bytes[1])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(encoded.bytes[2])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(encoded.bytes[3]));
    require(header_size == encoded.bytes.size() - cabinflow::gateway::kFrameHeaderBytes,
            "header must be the big-endian protobuf body size");
}

void test_split_header_and_body_are_buffered() {
    const auto encoded = RuntimeMessageFramer::encode(make_message("message-1", "payload"));
    require(static_cast<bool>(encoded), "encode split frame");

    RuntimeMessageFramer framer;
    require(framer.feed(std::string_view(encoded.bytes).substr(0, 1)).messages.empty(),
            "one header byte must not decode");
    require(framer.feed(std::string_view(encoded.bytes).substr(1, 2)).messages.empty(),
            "partial header must not decode");
    require(framer.feed(std::string_view(encoded.bytes).substr(3, 4)).messages.empty(),
            "partial body must not decode");

    const auto decoded = framer.feed(std::string_view(encoded.bytes).substr(7));
    require(decoded && decoded.messages.size() == 1,
            "complete split frame must decode once");
    require(decoded.messages.front().envelope.message_id == "message-1" &&
                decoded.messages.front().payload == "payload",
            "split frame content changed");
}

void test_coalesced_frames_preserve_order() {
    const auto first = RuntimeMessageFramer::encode(make_message("message-1", "first"));
    const auto second = RuntimeMessageFramer::encode(make_message("message-2", "second"));
    require(static_cast<bool>(first) && static_cast<bool>(second), "encode coalesced frames");

    RuntimeMessageFramer framer;
    const auto decoded = framer.feed(first.bytes + second.bytes);
    require(decoded && decoded.messages.size() == 2,
            "coalesced frames must decode together");
    require(decoded.messages[0].envelope.message_id == "message-1" &&
                decoded.messages[1].envelope.message_id == "message-2",
            "coalesced frame order changed");
}

void test_invalid_length_is_terminal() {
    std::string zero_length;
    append_u32_be(&zero_length, 0);

    RuntimeMessageFramer framer;
    require(framer.feed(zero_length).error == FrameError::kZeroBody,
            "zero length must be framing error");
    require(framer.feed("anything").error == FrameError::kZeroBody,
            "terminal framing error must not recover");

    std::string oversized;
    append_u32_be(&oversized, cabinflow::gateway::kMaxFrameBytes + 1U);
    RuntimeMessageFramer too_large_framer;
    require(too_large_framer.feed(oversized).error == FrameError::kBodyTooLarge,
            "oversized frame must fail before body arrives");
}

void test_malformed_protobuf_is_terminal() {
    std::string malformed;
    const std::string body = "not protobuf";
    append_u32_be(&malformed, static_cast<std::uint32_t>(body.size()));
    malformed.append(body);

    RuntimeMessageFramer framer;
    require(framer.feed(malformed).error == FrameError::kMalformedProtobuf,
            "malformed protobuf must be fatal");
}

}  // namespace

int main() {
    try {
        test_encoder_writes_big_endian_length_prefix();
        test_split_header_and_body_are_buffered();
        test_coalesced_frames_preserve_order();
        test_invalid_length_is_terminal();
        test_malformed_protobuf_is_terminal();
        std::cout << "gateway framer contract test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gateway framer contract test failed: " << error.what() << '\n';
        return 1;
    }
}
