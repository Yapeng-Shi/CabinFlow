#pragma once

#include <string_view>

#include <cabinflow/runtime/target_node.hpp>

namespace cabinflow::agent {

// 最小对话入口只确认文本消息已被类型化接收；推理配置不属于通用 Runtime 控制面。
class DialogueTextNode final : public runtime::TargetNode {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] runtime::RuntimeError start(
        runtime::NodeContext& context) override;
    void stop() noexcept override;

    [[nodiscard]] runtime::MessageHandlingResult on_message(
        const protocol::Message& message) noexcept override;

private:
    [[nodiscard]] runtime::MessageHandlingResult on_text_input(
        const protocol::MessageEnvelope& envelope,
        std::string_view text) noexcept;

    runtime::NodeContext* context_{nullptr};
};

}  // namespace cabinflow::agent
