#include <cabinflow/gateway/response_sequencer.hpp>

#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cabinflow::gateway {
namespace {

struct ResponseStreamKey {
    std::string session_id;
    std::string work_id;
    std::string topic;
    std::string registration_connection_id;

    [[nodiscard]] bool operator==(const ResponseStreamKey& other) const noexcept {
        return session_id == other.session_id && work_id == other.work_id &&
               topic == other.topic &&
               registration_connection_id == other.registration_connection_id;
    }
};

struct ResponseStreamKeyHash {
    [[nodiscard]] std::size_t operator()(const ResponseStreamKey& key) const {
        const auto session_hash = std::hash<std::string>{}(key.session_id);
        const auto work_hash = std::hash<std::string>{}(key.work_id);
        const auto topic_hash = std::hash<std::string>{}(key.topic);
        const auto connection_hash =
            std::hash<std::string>{}(key.registration_connection_id);
        return session_hash ^ (work_hash << 1U) ^ (topic_hash << 2U) ^
               (connection_hash << 3U);
    }
};

struct StreamState {
    std::uint64_t next_sequence{0};
    bool exhausted{false};
};

}  // namespace

struct ResponseSequencer::State {
    std::mutex mutex;
    std::unordered_map<ResponseStreamKey, StreamState, ResponseStreamKeyHash> streams;
};

ResponseSequencer::ResponseSequencer() : state_(std::make_unique<State>()) {}

ResponseSequencer::~ResponseSequencer() = default;

ResponseSequenceResult ResponseSequencer::next(
    std::string_view session_id, std::string_view work_id,
    std::string_view topic,
    std::string_view connection_id) {
    if (topic.empty() || (session_id.empty() && connection_id.empty())) {
        return {0, ResponseSequenceError::kMissingConnectionId};
    }

    // 只有无 session 的控制注册和错误流需要连接 ID 额外隔离。
    ResponseStreamKey key{std::string(session_id), std::string(work_id),
                          std::string(topic),
                          session_id.empty() ? std::string(connection_id) : ""};
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& stream = state_->streams[key];
    if (stream.exhausted) {
        return {0, ResponseSequenceError::kExhausted};
    }

    const auto sequence = stream.next_sequence;
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        stream.exhausted = true;
    } else {
        ++stream.next_sequence;
    }
    return {sequence, ResponseSequenceError::kNone};
}

}  // namespace cabinflow::gateway
