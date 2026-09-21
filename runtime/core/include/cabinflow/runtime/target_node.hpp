#pragma once

#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/node.hpp>

namespace cabinflow::runtime {

enum class MessageHandlingResult {
    kHandled,
    kUnsupportedTopic,
    kInvalidPayload,
};

// TargetNode 是 Runtime 数据面的唯一业务入口；Runtime 不认识 payload 的具体 Protobuf。
class TargetNode : public Node {
public:
    ~TargetNode() override = default;

    [[nodiscard]] virtual MessageHandlingResult on_message(
        const protocol::Message& message) noexcept = 0;
};

}  // namespace cabinflow::runtime
