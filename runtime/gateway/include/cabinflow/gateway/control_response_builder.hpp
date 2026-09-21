#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/gateway/response_sequencer.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>

namespace cabinflow::gateway {

enum class ControlResponseBuildError {
    kNone,
    kInvalidResponseTtl,
    kInvalidServiceResponse,
    kSequenceFailure,
    kMessageIdGenerationFailure,
    kSerializationFailure,
};

struct ControlResponseBuildResult {
    protocol::Message message;
    ControlResponseBuildError error{ControlResponseBuildError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ControlResponseBuildError::kNone;
    }
};

// 将控制结果封装成唯一的 control.response RuntimeMessage，不进入 SessionLedger。
class ControlResponseBuilder final {
public:
    ControlResponseBuilder(const runtime::Clock& clock,
                           ResponseSequencer& sequencer,
                           std::uint32_t response_ttl_ms);

    [[nodiscard]] ControlResponseBuildResult build(
        const ValidatedControlRequest& request,
        const ControlServiceResult& service_result,
        std::string_view connection_id) const;

private:
    const runtime::Clock& clock_;
    ResponseSequencer& sequencer_;
    std::uint32_t response_ttl_ms_;
};

}  // namespace cabinflow::gateway
