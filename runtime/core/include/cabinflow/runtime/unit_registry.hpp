#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cabinflow::runtime {

enum class WorkState {
    kRunning,
    kPaused,
    kExited,
};

enum class UnitState {
    kReady,
    kBusy,
    kStopping,
};

struct UnitRegistration {
    std::string unit_id;
    std::vector<std::string> capabilities;
    std::uint32_t max_concurrent_work{0};
};

struct WorkInfo {
    std::string work_id;
    std::string unit_id;
    WorkState state{WorkState::kRunning};
};

enum class UnitRegistryError {
    kNone,
    kInvalidUnit,
    kUnitAlreadyRegistered,
    kUnitNotFound,
    kUnitUnavailable,
    kWorkIdGenerationFailure,
    kWorkNotFound,
    kWorkNotRunning,
};

struct RegisterUnitResult {
    UnitRegistryError error{UnitRegistryError::kNone};
    UnitState state{UnitState::kReady};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == UnitRegistryError::kNone;
    }
};

struct WorkResult {
    UnitRegistryError error{UnitRegistryError::kNone};
    WorkInfo work;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == UnitRegistryError::kNone;
    }
};

// work 的创建、状态转换和容量计算只在此处持锁完成，避免控制层出现半创建状态。
class UnitRegistry final {
public:
    UnitRegistry();
    ~UnitRegistry();

    UnitRegistry(const UnitRegistry&) = delete;
    UnitRegistry& operator=(const UnitRegistry&) = delete;
    UnitRegistry(UnitRegistry&&) = delete;
    UnitRegistry& operator=(UnitRegistry&&) = delete;

    [[nodiscard]] RegisterUnitResult register_unit(UnitRegistration registration);
    [[nodiscard]] WorkResult create_work(std::string session_id,
                                         std::string unit_id);
    [[nodiscard]] WorkResult pause_work(std::string_view session_id,
                                        std::string_view work_id);
    [[nodiscard]] WorkResult exit_work(std::string_view session_id,
                                       std::string_view work_id);
    [[nodiscard]] WorkResult find_work(std::string_view session_id,
                                       std::string_view work_id) const;
    [[nodiscard]] std::vector<WorkInfo> find_session_work(
        std::string_view session_id) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace cabinflow::runtime
