#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <cabinflow/runtime/target_node.hpp>

namespace cabinflow::agent {

// 座舱文本节点只识别意图；车控执行不属于此节点。
class DialogueTextNode final : public runtime::TargetNode {
public:
    using OutputHandler = std::function<bool(
        const protocol::MessageEnvelope&, std::string_view, std::string)>;

    explicit DialogueTextNode(OutputHandler output_handler);

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

    OutputHandler output_handler_;
    std::map<std::pair<std::string, std::string>, std::string> buffered_inputs_;
    runtime::NodeContext* context_{nullptr};
};

}  // namespace cabinflow::agent
