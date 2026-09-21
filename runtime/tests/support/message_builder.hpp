#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <cabinflow/protocol/message.hpp>

namespace cabinflow::test {

class MessageBuilder final {
public:
    MessageBuilder() {
        message_.envelope.message_id = "test-message";
        message_.envelope.trace_id = "test-trace";
        message_.envelope.session_id = "driver-session";
        message_.envelope.work_id = "test-work";
        message_.envelope.source_node = "test-source";
        message_.envelope.target_node = "test-processor";
        message_.envelope.topic = "test.topic";
        message_.envelope.created_monotonic_ns = 1'000'000;
        message_.envelope.ttl_ms = 100;
    }

    MessageBuilder& with_topic(std::string_view topic) {
        message_.envelope.topic = topic;
        return *this;
    }

    MessageBuilder& with_payload(std::string_view payload) {
        message_.payload = payload;
        return *this;
    }

    MessageBuilder& with_message_id(std::string_view message_id) {
        message_.envelope.message_id = message_id;
        return *this;
    }

    MessageBuilder& with_work_id(std::string_view work_id) {
        message_.envelope.work_id = work_id;
        return *this;
    }

    MessageBuilder& with_created_monotonic_ns(std::uint64_t timestamp) {
        message_.envelope.created_monotonic_ns = timestamp;
        return *this;
    }

    [[nodiscard]] protocol::Message build() const { return message_; }

private:
    protocol::Message message_;
};

}  // namespace cabinflow::test
