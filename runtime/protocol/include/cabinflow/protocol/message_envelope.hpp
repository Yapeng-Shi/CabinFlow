#pragma once

#include <cstdint>
#include <string>

#include <cabinflow/protocol/message_kind.hpp>

namespace cabinflow::protocol {

inline constexpr std::uint16_t kCurrentSchemaVersion = 1;

enum class SeatPosition {
    kUnknown,
    kDriver,
    kFrontPassenger,
    kRearLeft,
    kRearCenter,
    kRearRight,
};

// Transport-neutral metadata for a runtime message. Payload serialization is
// intentionally outside this type so the same envelope can be used by ZMQ,
// IPC, and future vehicle-service adapters.
struct MessageEnvelope {
    std::uint16_t schema_version{kCurrentSchemaVersion};
    std::string message_id;
    std::string trace_id;
    std::string session_id;
    std::string work_id;
    std::string source_node;
    std::string target_node;
    std::string topic;
    MessageKind kind{MessageKind::kData};
    SeatPosition seat{SeatPosition::kUnknown};
    std::uint64_t sequence{0};
    std::uint64_t created_monotonic_ns{0};
    std::uint32_t ttl_ms{0};
    bool is_final{false};
};

}  // namespace cabinflow::protocol
