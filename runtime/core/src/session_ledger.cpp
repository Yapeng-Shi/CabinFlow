#include <cabinflow/runtime/session_ledger.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cabinflow::runtime {
namespace {

constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;

struct StreamKey {
    std::string session_id;
    std::string work_id;
    std::string topic;

    [[nodiscard]] bool operator==(const StreamKey& other) const noexcept {
        return session_id == other.session_id && work_id == other.work_id &&
               topic == other.topic;
    }
};

struct WorkKey {
    std::string session_id;
    std::string work_id;

    [[nodiscard]] bool operator==(const WorkKey& other) const noexcept {
        return session_id == other.session_id && work_id == other.work_id;
    }
};

template <typename Key>
void hash_combine(std::size_t& seed, const Key& value) {
    seed ^= std::hash<Key>{}(value) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
}

struct StreamKeyHash {
    [[nodiscard]] std::size_t operator()(const StreamKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, key.session_id);
        hash_combine(seed, key.work_id);
        hash_combine(seed, key.topic);
        return seed;
    }
};

struct WorkKeyHash {
    [[nodiscard]] std::size_t operator()(const WorkKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, key.session_id);
        hash_combine(seed, key.work_id);
        return seed;
    }
};

struct StreamState {
    std::uint64_t last_sequence{0};
    bool has_sequence{false};
    bool finalized{false};
};

[[nodiscard]] bool has_required_identity(
    const protocol::MessageEnvelope& envelope) {
    return !envelope.message_id.empty() && !envelope.trace_id.empty() &&
           !envelope.session_id.empty() && !envelope.work_id.empty() &&
           !envelope.source_node.empty() && !envelope.target_node.empty() &&
           !envelope.topic.empty();
}

}  // namespace

struct SessionLedger::State {
    std::mutex mutex;
    std::unordered_set<std::string> observed_message_ids;
    std::unordered_map<StreamKey, StreamState, StreamKeyHash> streams;
    std::unordered_set<std::string> cancelled_sessions;
    std::unordered_set<WorkKey, WorkKeyHash> cancelled_work;
};

SessionLedger::SessionLedger(const Clock& clock)
    : clock_(clock), state_(std::make_unique<State>()) {}

SessionLedger::~SessionLedger() = default;

DeliveryResult SessionLedger::observe(
    const protocol::MessageEnvelope& envelope) {
    const auto now_monotonic_ns = clock_.now_monotonic_ns();
    if (envelope.schema_version != protocol::kCurrentSchemaVersion) {
        return DeliveryResult::kUnsupportedSchemaVersion;
    }

    if (!has_required_identity(envelope) || envelope.ttl_ms == 0 ||
        now_monotonic_ns < envelope.created_monotonic_ns) {
        return DeliveryResult::kInvalidEnvelope;
    }

    const auto ttl_ns = static_cast<std::uint64_t>(envelope.ttl_ms) *
                        kNanosecondsPerMillisecond;
    if (now_monotonic_ns - envelope.created_monotonic_ns >= ttl_ns) {
        return DeliveryResult::kExpired;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->cancelled_sessions.count(envelope.session_id) != 0U) {
        return DeliveryResult::kSessionCancelled;
    }

    const WorkKey work_key{envelope.session_id, envelope.work_id};
    if (state_->cancelled_work.count(work_key) != 0U) {
        return DeliveryResult::kWorkCancelled;
    }

    if (state_->observed_message_ids.count(envelope.message_id) != 0U) {
        return DeliveryResult::kDuplicateMessage;
    }

    const StreamKey stream_key{envelope.session_id, envelope.work_id,
                               envelope.topic};
    auto& stream = state_->streams[stream_key];
    if (stream.finalized) {
        return DeliveryResult::kStreamFinalized;
    }

    if (stream.has_sequence && envelope.sequence <= stream.last_sequence) {
        return DeliveryResult::kStaleSequence;
    }

    stream.last_sequence = envelope.sequence;
    stream.has_sequence = true;
    stream.finalized = envelope.is_final;
    state_->observed_message_ids.insert(envelope.message_id);
    return DeliveryResult::kAccepted;
}

void SessionLedger::cancel_session(std::string_view session_id) {
    if (session_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->cancelled_sessions.emplace(session_id);
}

void SessionLedger::cancel_work(std::string_view session_id,
                                std::string_view work_id) {
    if (session_id.empty() || work_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->cancelled_work.emplace(WorkKey{std::string(session_id),
                                           std::string(work_id)});
}

std::string_view to_string(DeliveryResult result) noexcept {
    switch (result) {
        case DeliveryResult::kAccepted:
            return "accepted";
        case DeliveryResult::kInvalidEnvelope:
            return "invalid_envelope";
        case DeliveryResult::kUnsupportedSchemaVersion:
            return "unsupported_schema_version";
        case DeliveryResult::kExpired:
            return "expired";
        case DeliveryResult::kDuplicateMessage:
            return "duplicate_message";
        case DeliveryResult::kStaleSequence:
            return "stale_sequence";
        case DeliveryResult::kStreamFinalized:
            return "stream_finalized";
        case DeliveryResult::kSessionCancelled:
            return "session_cancelled";
        case DeliveryResult::kWorkCancelled:
            return "work_cancelled";
    }

    return "unknown";
}

}  // namespace cabinflow::runtime
