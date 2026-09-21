#pragma once

#include <cstdint>
#include <string_view>

#include <delivery.pb.h>

#include <cabinflow/gateway/response_sequencer.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>

namespace cabinflow::gateway {

enum class DeliveryErrorBuildError {
    kNone,
    kInvalidResponseTtl,
    kSequenceFailure,
    kMessageIdGenerationFailure,
    kSerializationFailure,
};

struct DeliveryErrorBuildResult {
    protocol::Message message;
    DeliveryErrorBuildError error{DeliveryErrorBuildError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == DeliveryErrorBuildError::kNone;
    }
};

// DeliveryError 是数据面的唯一拒绝响应，不进入 SessionLedger。
class DeliveryErrorBuilder final {
public:
    DeliveryErrorBuilder(const runtime::Clock& clock, ResponseSequencer& sequencer,
                         std::uint32_t response_ttl_ms);

    [[nodiscard]] DeliveryErrorBuildResult build(
        const protocol::Message& request,
        protocol::v1::DeliveryErrorCode code, std::string_view detail,
        std::string_view connection_id) const;

private:
    const runtime::Clock& clock_;
    ResponseSequencer& sequencer_;
    std::uint32_t response_ttl_ms_;
};

}  // namespace cabinflow::gateway
