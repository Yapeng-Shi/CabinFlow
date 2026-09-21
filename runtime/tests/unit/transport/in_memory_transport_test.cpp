#include <iostream>
#include <string_view>

#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "message_builder.hpp"

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

cabinflow::protocol::Message make_message(std::string_view topic) {
    return cabinflow::test::MessageBuilder()
        .with_topic(topic)
        .with_payload("hello")
        .build();
}

}  // namespace

int main() {
    using cabinflow::transport::InMemoryTransport;
    using cabinflow::transport::TransportError;

    InMemoryTransport transport;
    int matching_deliveries = 0;
    int other_deliveries = 0;
    std::string_view callback_message_id;

    auto matching = transport.subscribe(
        "voice.partial", [&](const auto& message) {
            ++matching_deliveries;
            callback_message_id = message.envelope.message_id;
        });
    auto other = transport.subscribe("voice.final", [&](const auto&) {
        ++other_deliveries;
    });

    if (!expect(static_cast<bool>(matching), "matching subscription") ||
        !expect(static_cast<bool>(other), "other subscription") ||
        !expect(matching.subscription->active(), "active subscription") ||
        !expect(transport.publish(make_message("voice.partial")) ==
                    TransportError::kNone,
                "publish succeeds") ||
        !expect(matching_deliveries == 1, "matching topic delivered") ||
        !expect(other_deliveries == 0, "non-matching topic skipped") ||
        !expect(callback_message_id == "test-message", "callback sees envelope")) {
        return 1;
    }

    matching.subscription->unsubscribe();
    if (!expect(!matching.subscription->active(), "subscription inactive") ||
        !expect(transport.publish(make_message("voice.partial")) ==
                    TransportError::kNone,
                "publish after unsubscribe") ||
        !expect(matching_deliveries == 1, "unsubscribed handler not called")) {
        return 1;
    }

    int scoped_deliveries = 0;
    {
        auto scoped = transport.subscribe("voice.scoped", [&](const auto&) {
            ++scoped_deliveries;
        });
        if (!expect(static_cast<bool>(scoped), "scoped subscription")) {
            return 1;
        }
    }
    if (!expect(transport.publish(make_message("voice.scoped")) ==
                    TransportError::kNone,
                "publish after subscription destruction") ||
        !expect(scoped_deliveries == 0,
                "destroyed subscription handler not called")) {
        return 1;
    }

    const auto invalid_topic = transport.subscribe("", [](const auto&) {});
    const auto empty_handler = transport.subscribe("voice.partial", {});
    if (!expect(invalid_topic.error == TransportError::kInvalidTopic,
                "invalid subscription topic") ||
        !expect(empty_handler.error == TransportError::kEmptyHandler,
                "empty handler rejected") ||
        !expect(transport.publish(make_message("")) ==
                    TransportError::kInvalidTopic,
                "invalid publish topic")) {
        return 1;
    }

    return 0;
}
