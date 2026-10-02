#include <array>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

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

bool test_rejected_envelope_matrix() {
    using cabinflow::protocol::MessageEnvelope;
    using cabinflow::runtime::DeliveryResult;
    using cabinflow::runtime::SessionLedger;

    struct Case {
        std::string_view name;
        void (*mutate)(MessageEnvelope&);
        DeliveryResult expected;
    };
    const std::array<Case, 11> cases{{
        {"missing message_id", +[](MessageEnvelope& value) { value.message_id.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing trace_id", +[](MessageEnvelope& value) { value.trace_id.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing session_id", +[](MessageEnvelope& value) { value.session_id.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing work_id", +[](MessageEnvelope& value) { value.work_id.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing source_node", +[](MessageEnvelope& value) { value.source_node.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing target_node", +[](MessageEnvelope& value) { value.target_node.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"missing topic", +[](MessageEnvelope& value) { value.topic.clear(); },
         DeliveryResult::kInvalidEnvelope},
        {"zero ttl", +[](MessageEnvelope& value) { value.ttl_ms = 0; },
         DeliveryResult::kInvalidEnvelope},
        {"future timestamp", +[](MessageEnvelope& value) {
             value.created_monotonic_ns = kNowNs + 1;
         }, DeliveryResult::kInvalidEnvelope},
        {"expired at ttl boundary", +[](MessageEnvelope& value) {
             value.created_monotonic_ns = kNowNs - 100'000'000ULL;
         }, DeliveryResult::kExpired},
        {"unsupported schema", +[](MessageEnvelope& value) {
             value.schema_version = 2;
         }, DeliveryResult::kUnsupportedSchemaVersion},
    }};

    for (const auto& test : cases) {
        cabinflow::test::FakeClock clock(kNowNs);
        SessionLedger ledger(clock);
        auto rejected = make_envelope("matrix-id", "session", "work", 0);
        test.mutate(rejected);
        if (!expect(ledger.observe(rejected), test.expected, test.name)) {
            return false;
        }
        // 拒绝不能占用 message_id 或 sequence：修正同一条消息后仍可准入。
        const auto corrected = make_envelope("matrix-id", "session", "work", 0);
        if (!expect(ledger.observe(corrected), DeliveryResult::kAccepted,
                    "rejected envelope did not mutate ledger")) {
            return false;
        }
    }
    return true;
}

bool test_cancel_races_with_input(bool is_final) {
    using cabinflow::runtime::DeliveryResult;
    using cabinflow::runtime::SessionLedger;

    for (int attempt = 0; attempt != 64; ++attempt) {
        cabinflow::test::FakeClock clock(kNowNs);
        SessionLedger ledger(clock);
        std::mutex start_mutex;
        std::condition_variable start_changed;
        int ready = 0;
        bool released = false;
        const auto start_together = [&] {
            std::unique_lock<std::mutex> lock(start_mutex);
            ++ready;
            start_changed.notify_all();
            start_changed.wait(lock, [&] { return released; });
        };

        auto racing = make_envelope("racing", "session", "work", 0);
        racing.is_final = is_final;
        DeliveryResult result = DeliveryResult::kInvalidEnvelope;
        std::thread input([&] {
            start_together();
            result = ledger.observe(racing);
        });
        std::thread cancel([&] {
            start_together();
            ledger.cancel_work("session", "work");
        });
        {
            std::unique_lock<std::mutex> lock(start_mutex);
            start_changed.wait(lock, [&] { return ready == 2; });
            released = true;
        }
        start_changed.notify_all();
        input.join();
        cancel.join();

        // 两种线性化顺序都允许；取消完成后不能因 partial/final 竞态重新开放 work。
        if (result != DeliveryResult::kAccepted &&
            result != DeliveryResult::kWorkCancelled) {
            return false;
        }
        if (!expect(ledger.observe(make_envelope("after", "session", "work", 1)),
                    DeliveryResult::kWorkCancelled,
                    "cancel and input race leaves work cancelled") ||
            !expect(ledger.observe(make_envelope("other", "other-session", "work", 0)),
                    DeliveryResult::kAccepted,
                    "cancel and input race preserves session isolation")) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    using cabinflow::runtime::DeliveryResult;
    using cabinflow::runtime::SessionLedger;

    if (!test_rejected_envelope_matrix()) {
        return 1;
    }
    if (!test_cancel_races_with_input(false) ||
        !test_cancel_races_with_input(true)) {
        return 1;
    }

    cabinflow::test::FakeClock clock(kNowNs);
    SessionLedger ledger(clock);
    auto first = make_envelope("msg-1", "driver-session", "music-work", 0);
    if (!expect(ledger.observe(first), DeliveryResult::kAccepted,
                "first message") ||
        !expect(ledger.observe(first),
                DeliveryResult::kDuplicateMessage, "duplicate message")) {
        return 1;
    }

    auto duplicate_across_sessions =
        make_envelope("msg-1", "passenger-session", "other-work", 0);
    if (!expect(ledger.observe(duplicate_across_sessions),
                DeliveryResult::kDuplicateMessage,
                "message id is process-wide, not session-local")) {
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

    ledger.cancel_work("driver-session", "music-work");
    auto post_final_cancel =
        make_envelope("msg-after-final-cancel", "driver-session", "music-work", 4);
    if (!expect(ledger.observe(post_final_cancel), DeliveryResult::kWorkCancelled,
                "work cancellation takes precedence after final")) {
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
