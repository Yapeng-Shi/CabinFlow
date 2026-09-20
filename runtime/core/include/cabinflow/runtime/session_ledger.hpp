#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <cabinflow/protocol/message_envelope.hpp>
#include <cabinflow/runtime/clock.hpp>

namespace cabinflow::runtime {

enum class DeliveryResult {
    kAccepted,
    kInvalidEnvelope,
    kUnsupportedSchemaVersion,
    kExpired,
    kDuplicateMessage,
    kStaleSequence,
    kStreamFinalized,
    kSessionCancelled,
    kWorkCancelled,
};

[[nodiscard]] std::string_view to_string(DeliveryResult result) noexcept;

// An in-memory, thread-safe admission gate for one runtime process. It owns
// idempotency, ordering, final-message, and cancellation state; it does not
// own payload buffers or dispatch work to handlers.
class SessionLedger final {
public:
    explicit SessionLedger(const Clock& clock);
    ~SessionLedger();

    SessionLedger(const SessionLedger&) = delete;
    SessionLedger& operator=(const SessionLedger&) = delete;
    SessionLedger(SessionLedger&&) = delete;
    SessionLedger& operator=(SessionLedger&&) = delete;

    [[nodiscard]] DeliveryResult observe(
        const protocol::MessageEnvelope& envelope);

    void cancel_session(std::string_view session_id);
    void cancel_work(std::string_view session_id, std::string_view work_id);

private:
    const Clock& clock_;
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace cabinflow::runtime
