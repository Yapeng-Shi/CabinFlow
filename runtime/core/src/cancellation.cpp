#include <cabinflow/runtime/cancellation.hpp>

#include <mutex>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

namespace cabinflow::runtime::detail {
namespace {

struct WorkKey {
    std::string session_id;
    std::string work_id;

    [[nodiscard]] bool operator==(const WorkKey& other) const noexcept {
        return session_id == other.session_id && work_id == other.work_id;
    }
};

struct WorkKeyHash {
    [[nodiscard]] std::size_t operator()(const WorkKey& key) const {
        const auto first = std::hash<std::string>{}(key.session_id);
        const auto second = std::hash<std::string>{}(key.work_id);
        return first ^ (second + 0x9e3779b9U + (first << 6U) +
                        (first >> 2U));
    }
};

}  // namespace

struct CancellationState {
    std::mutex mutex;
    std::unordered_set<std::string> cancelled_sessions;
    std::unordered_set<WorkKey, WorkKeyHash> cancelled_work;
};

}  // namespace cabinflow::runtime::detail

namespace cabinflow::runtime {

CancellationToken::CancellationToken(
    std::shared_ptr<detail::CancellationState> state, std::string session_id,
    std::string work_id)
    : state_(std::move(state)),
      session_id_(std::move(session_id)),
      work_id_(std::move(work_id)) {}

bool CancellationToken::valid() const noexcept {
    return state_ != nullptr && !session_id_.empty() && !work_id_.empty();
}

bool CancellationToken::cancelled() const noexcept {
    return reason() != CancellationReason::kNone;
}

CancellationReason CancellationToken::reason() const noexcept {
    if (!valid()) {
        return CancellationReason::kNone;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    // session 级取消覆盖该 session 下所有 work；work 级取消只影响精确 pair。
    if (state_->cancelled_sessions.count(session_id_) != 0U) {
        return CancellationReason::kSession;
    }
    if (state_->cancelled_work.count({session_id_, work_id_}) != 0U) {
        return CancellationReason::kWork;
    }
    return CancellationReason::kNone;
}

CancellationRegistry::CancellationRegistry()
    : state_(std::make_shared<detail::CancellationState>()) {}

CancellationRegistry::~CancellationRegistry() = default;

CancellationToken CancellationRegistry::token_for(
    std::string_view session_id, std::string_view work_id) const {
    if (session_id.empty() || work_id.empty()) {
        return {};
    }
    return {state_, std::string(session_id), std::string(work_id)};
}

bool CancellationRegistry::cancel_session(std::string_view session_id) {
    if (session_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cancelled_sessions.emplace(session_id).second;
}

bool CancellationRegistry::cancel_work(std::string_view session_id,
                                       std::string_view work_id) {
    if (session_id.empty() || work_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cancelled_work
        .emplace(detail::WorkKey{std::string(session_id), std::string(work_id)})
        .second;
}

}  // namespace cabinflow::runtime
