#pragma once

#include <cstdint>
#include <memory>

#include <control.pb.h>

#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/runtime/unit_registry.hpp>

namespace cabinflow::gateway {

enum class ControlErrorCode : std::uint32_t {
    kInvalidRequest = 1,
    kConflict = 2,
    kUnitNotFound = 3,
    kUnitUnavailable = 4,
    kWorkNotFound = 5,
    kInvalidWorkState = 6,
    kInternal = 7,
    kUnsupportedSchemaVersion = 8,
    kInvalidEnvelope = 9,
};

struct ControlServiceResult {
    protocol::v1::ControlResponse response;
};

// ControlService 只管理控制命令与 work 的创建/转换；数据面准入仍由 SessionLedger 负责。
class ControlService final {
public:
    explicit ControlService(runtime::UnitRegistry& registry);
    ~ControlService();

    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;
    ControlService(ControlService&&) = delete;
    ControlService& operator=(ControlService&&) = delete;

    [[nodiscard]] ControlServiceResult handle(
        const ValidatedControlRequest& request);

private:
    runtime::UnitRegistry& registry_;
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace cabinflow::gateway
