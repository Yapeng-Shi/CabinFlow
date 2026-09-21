#include <cabinflow/runtime/unit_registry.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <sys/random.h>

namespace cabinflow::runtime {
namespace {

struct UnitRecord {
    std::vector<std::string> capabilities;
    std::uint32_t max_concurrent_work{0};
    std::uint32_t active_work_count{0};
    UnitState state{UnitState::kReady};
};

struct WorkRecord {
    std::string session_id;
    WorkInfo info;
};

void refresh_unit_state(UnitRecord& unit) {
    if (unit.state == UnitState::kStopping) {
        return;
    }
    unit.state = unit.active_work_count >= unit.max_concurrent_work
                     ? UnitState::kBusy
                     : UnitState::kReady;
}

[[nodiscard]] std::string generate_work_id() {
    std::array<unsigned char, 16> random_bytes{};
    // Linux getrandom 提供不可预测的服务端身份；失败就明确拒绝本次 setup。
    if (::getrandom(random_bytes.data(), random_bytes.size(), 0) !=
        static_cast<ssize_t>(random_bytes.size())) {
        return {};
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string work_id{"work-"};
    work_id.reserve(work_id.size() + random_bytes.size() * 2U);
    for (const auto byte : random_bytes) {
        work_id.push_back(kHex[(byte >> 4U) & 0x0fU]);
        work_id.push_back(kHex[byte & 0x0fU]);
    }
    return work_id;
}

}  // namespace

struct UnitRegistry::State {
    mutable std::mutex mutex;
    std::unordered_map<std::string, UnitRecord> units;
    std::unordered_map<std::string, WorkRecord> work_by_id;
};

UnitRegistry::UnitRegistry() : state_(std::make_unique<State>()) {}

UnitRegistry::~UnitRegistry() = default;

RegisterUnitResult UnitRegistry::register_unit(UnitRegistration registration) {
    if (registration.unit_id.empty() || registration.max_concurrent_work == 0U) {
        return {UnitRegistryError::kInvalidUnit, UnitState::kReady};
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->units.count(registration.unit_id) != 0U) {
        return {UnitRegistryError::kUnitAlreadyRegistered, UnitState::kReady};
    }

    state_->units.emplace(
        std::move(registration.unit_id),
        UnitRecord{std::move(registration.capabilities),
                   registration.max_concurrent_work, 0, UnitState::kReady});
    return {UnitRegistryError::kNone, UnitState::kReady};
}

WorkResult UnitRegistry::create_work(std::string session_id, std::string unit_id) {
    if (session_id.empty() || unit_id.empty()) {
        return {UnitRegistryError::kInvalidUnit, {}};
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto unit_it = state_->units.find(unit_id);
    if (unit_it == state_->units.end()) {
        return {UnitRegistryError::kUnitNotFound, {}};
    }

    auto& unit = unit_it->second;
    if (unit.state == UnitState::kStopping ||
        unit.active_work_count >= unit.max_concurrent_work) {
        return {UnitRegistryError::kUnitUnavailable, {}};
    }

    const auto work_id = generate_work_id();
    if (work_id.empty() || state_->work_by_id.count(work_id) != 0U) {
        return {UnitRegistryError::kWorkIdGenerationFailure, {}};
    }

    WorkInfo work{work_id, std::move(unit_id), WorkState::kRunning};
    state_->work_by_id.emplace(work_id, WorkRecord{std::move(session_id), work});
    ++unit.active_work_count;
    refresh_unit_state(unit);
    return {UnitRegistryError::kNone, std::move(work)};
}

WorkResult UnitRegistry::pause_work(std::string_view session_id,
                                    std::string_view work_id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto work_it = state_->work_by_id.find(std::string(work_id));
    if (work_it == state_->work_by_id.end() || work_it->second.session_id != session_id) {
        return {UnitRegistryError::kWorkNotFound, {}};
    }
    if (work_it->second.info.state != WorkState::kRunning) {
        return {UnitRegistryError::kWorkNotRunning, {}};
    }

    work_it->second.info.state = WorkState::kPaused;
    return {UnitRegistryError::kNone, work_it->second.info};
}

WorkResult UnitRegistry::exit_work(std::string_view session_id,
                                   std::string_view work_id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto work_it = state_->work_by_id.find(std::string(work_id));
    if (work_it == state_->work_by_id.end() || work_it->second.session_id != session_id) {
        return {UnitRegistryError::kWorkNotFound, {}};
    }
    if (work_it->second.info.state == WorkState::kExited) {
        return {UnitRegistryError::kWorkNotRunning, {}};
    }

    const auto unit_it = state_->units.find(work_it->second.info.unit_id);
    if (unit_it == state_->units.end() || unit_it->second.active_work_count == 0U) {
        return {UnitRegistryError::kUnitUnavailable, {}};
    }

    work_it->second.info.state = WorkState::kExited;
    --unit_it->second.active_work_count;
    refresh_unit_state(unit_it->second);
    return {UnitRegistryError::kNone, work_it->second.info};
}

WorkResult UnitRegistry::find_work(std::string_view session_id,
                                   std::string_view work_id) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto work_it = state_->work_by_id.find(std::string(work_id));
    if (work_it == state_->work_by_id.end() || work_it->second.session_id != session_id) {
        return {UnitRegistryError::kWorkNotFound, {}};
    }
    return {UnitRegistryError::kNone, work_it->second.info};
}

std::vector<WorkInfo> UnitRegistry::find_session_work(
    std::string_view session_id) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<WorkInfo> works;
    for (const auto& [_, record] : state_->work_by_id) {
        if (record.session_id == session_id) {
            works.push_back(record.info);
        }
    }
    std::sort(works.begin(), works.end(), [](const WorkInfo& left,
                                             const WorkInfo& right) {
        return left.work_id < right.work_id;
    });
    return works;
}

}  // namespace cabinflow::runtime
