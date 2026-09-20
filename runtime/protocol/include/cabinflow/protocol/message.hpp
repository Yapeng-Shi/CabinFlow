#pragma once

#include <string>

#include <cabinflow/protocol/message_envelope.hpp>

namespace cabinflow::protocol {

// Payload stays outside MessageEnvelope so Runtime can validate transport
// metadata without depending on an Agent-specific payload schema.
struct Message {
    MessageEnvelope envelope;
    std::string payload;
};

}  // namespace cabinflow::protocol
