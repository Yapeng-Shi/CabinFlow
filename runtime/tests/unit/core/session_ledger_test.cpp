#include <cstdint>
#include <iostream>
#include <string_view>

#include <cabinflow/runtime/session_ledger.hpp>

#include "fake_clock.hpp"

namespace {

constexpr std::uint64_t kNowNs = 5'000'000'000ULL;

cabinflow::protocol::MessageEnvelope make_envelope(
    std::string_view message_id, std::string_view session_id,
    std::string_view work_id, std::uint64_t sequence) {
    using cabinflow::protocol::MessageEnvelope;
    using cabinflow::protocol::SeatPosition;

    MessageEnvelope envelope;
    envelope.message_id = message_id;
    envelope.trace_id = "trace-001";
    envelope.session_id = session_id;
    envelope.work_id = work_id;
    envelope.source_node = "asr";
    envelope.target_node = "orchestrator";
    envelope.topic = "voice.partial";
    envelope.seat = SeatPosition::kDriver;
    envelope.sequence = sequence;
    envelope.created_monotonic_ns = kNowNs;
    envelope.ttl_ms = 100;
    return envelope;
}

bool expect(cabinflow::runtime::DeliveryResult actual,
            cabinflow::runtime::DeliveryResult expected,
            std::string_view name) {
    if (actual == expected) {
        return true;
    }

    std::cerr << name << ": expected "
              << cabinflow::runtime::to_string(expected) << ", got "
              << cabinflow::runtime::to_string(actual) << '\n';
    return false;
}

}  // namespace

int main() {
    using cabinflow::runtime::DeliveryResult;
    using cabinflow::runtime::SessionLedger;

    cabinflow::test::FakeClock clock(kNowNs);
    SessionLedger ledger(clock);
    auto first = make_envelope("msg-1", "driver-session", "music-work", 0);
    if (!expect(ledger.observe(first), DeliveryResult::kAccepted,
                "first message") ||
        !expect(ledger.observe(first),
                DeliveryResult::kDuplicateMessage, "duplicate message")) {
        return 1;
    }

    auto next = make_envelope("msg-2", "driver-session", "music-work", 1);
    if (!expect(ledger.observe(next), DeliveryResult::kAccepted,
                "monotonic sequence")) {
        return 1;
    }

    auto stale = make_envelope("msg-3", "driver-session", "music-work", 1);
    if (!expect(ledger.observe(stale), DeliveryResult::kStaleSequence,
                "stale sequence")) {
        return 1;
    }

    auto final = make_envelope("msg-4", "driver-session", "music-work", 2);
    final.is_final = true;
    if (!expect(ledger.observe(final), DeliveryResult::kAccepted,
                "final message")) {
        return 1;
    }

    auto post_final =
        make_envelope("msg-5", "driver-session", "music-work", 3);
    if (!expect(ledger.observe(post_final),
                DeliveryResult::kStreamFinalized, "post-final message")) {
        return 1;
    }

    auto invalid = make_envelope("msg-6", "driver-session", "nav-work", 0);
    invalid.topic.clear();
    if (!expect(ledger.observe(invalid),
                DeliveryResult::kInvalidEnvelope, "invalid envelope")) {
        return 1;
    }

    auto unsupported =
        make_envelope("msg-7", "driver-session", "nav-work", 0);
    unsupported.schema_version = 2;
    if (!expect(ledger.observe(unsupported),
                DeliveryResult::kUnsupportedSchemaVersion,
                "unsupported schema")) {
        return 1;
    }

    ledger.cancel_work("driver-session", "route-work");
    auto cancelled_work =
        make_envelope("msg-8", "driver-session", "route-work", 0);
    if (!expect(ledger.observe(cancelled_work),
                DeliveryResult::kWorkCancelled, "work cancellation")) {
        return 1;
    }

    auto passenger_same_work =
        make_envelope("msg-9", "passenger-session", "route-work", 0);
    if (!expect(ledger.observe(passenger_same_work),
                DeliveryResult::kAccepted, "work cancellation isolation")) {
        return 1;
    }

    ledger.cancel_session("driver-session");
    auto cancelled_session =
        make_envelope("msg-10", "driver-session", "climate-work", 0);
    if (!expect(ledger.observe(cancelled_session),
                DeliveryResult::kSessionCancelled, "session cancellation")) {
        return 1;
    }

    auto passenger_after_cancel =
        make_envelope("msg-11", "passenger-session", "climate-work", 0);
    if (!expect(ledger.observe(passenger_after_cancel),
                DeliveryResult::kAccepted, "session cancellation isolation")) {
        return 1;
    }

    auto expired = make_envelope("msg-12", "passenger-session", "nav-work", 0);
    expired.ttl_ms = 10;
    clock.advance(10'000'000ULL);
    if (!expect(ledger.observe(expired), DeliveryResult::kExpired,
                "expired message")) {
        return 1;
    }

    return 0;
}
