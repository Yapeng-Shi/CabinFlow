#include <cabinflow/agent/dialogue_text_node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include <cockpit_text.pb.h>

#include <cabinflow/observability/event.hpp>

namespace cabinflow::agent {
namespace {

constexpr std::size_t kMaxTextBytes = 16U * 1024U;

[[nodiscard]] bool is_continuation_byte(unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            length = 2;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            length = 3;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            length = 4;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if (!is_continuation_byte(next)) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

}  // namespace

std::string_view DialogueTextNode::name() const noexcept {
    return "dialogue.primary";
}

runtime::RuntimeError DialogueTextNode::start(runtime::NodeContext& context) {
    context_ = &context;
    return runtime::RuntimeError::kNone;
}

void DialogueTextNode::stop() noexcept { context_ = nullptr; }

runtime::MessageHandlingResult DialogueTextNode::on_message(
    const protocol::Message& message) noexcept {
    if (message.envelope.topic != "cockpit.text.input") {
        return runtime::MessageHandlingResult::kUnsupportedTopic;
    }

    v1::TextInput input;
    if (!input.ParseFromString(message.payload) || input.text().empty() ||
        input.text().size() > kMaxTextBytes || !is_valid_utf8(input.text())) {
        return runtime::MessageHandlingResult::kInvalidPayload;
    }
    return on_text_input(message.envelope, input.text());
}

runtime::MessageHandlingResult DialogueTextNode::on_text_input(
    const protocol::MessageEnvelope& envelope, std::string_view text) noexcept {
    if (context_ == nullptr) {
        return runtime::MessageHandlingResult::kInvalidPayload;
    }

    // 不记录原始座舱文本，避免最小运行时路径把用户内容扩散到日志。
    context_->logger().log(observability::Event{
        "dialogue", "text_input_received", envelope.trace_id, envelope.session_id,
        envelope.work_id, envelope.message_id,
        "text_bytes=" + std::to_string(text.size())});
    return runtime::MessageHandlingResult::kHandled;
}

}  // namespace cabinflow::agent
