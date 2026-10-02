#include <cabinflow/agent/dialogue_text_node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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

[[nodiscard]] std::pair<v1::CockpitIntent, std::string_view> recognize_intent(
    std::string_view text) noexcept {
    if (text == "打开空调") {
        return {v1::COCKPIT_INTENT_CLIMATE_ON, "识别到打开空调意图，未执行车控。"};
    }
    if (text == "关闭空调") {
        return {v1::COCKPIT_INTENT_CLIMATE_OFF, "识别到关闭空调意图，未执行车控。"};
    }
    if (text == "打开座椅加热" || text == "turn on seat heating") {
        return {v1::COCKPIT_INTENT_SEAT_HEATING_ON,
                "识别到打开座椅加热意图，未执行车控。"};
    }
    if (text == "关闭座椅加热") {
        return {v1::COCKPIT_INTENT_SEAT_HEATING_OFF,
                "识别到关闭座椅加热意图，未执行车控。"};
    }
    return {v1::COCKPIT_INTENT_UNRECOGNIZED,
            "未识别到支持的座舱意图，未执行车控。"};
}

}  // namespace

DialogueTextNode::DialogueTextNode(OutputHandler output_handler)
    : output_handler_(std::move(output_handler)) {}

std::string_view DialogueTextNode::name() const noexcept {
    return "dialogue.primary";
}

runtime::RuntimeError DialogueTextNode::start(runtime::NodeContext& context) {
    if (!output_handler_) {
        return runtime::RuntimeError::kNodeStartFailure;
    }
    context_ = &context;
    return runtime::RuntimeError::kNone;
}

void DialogueTextNode::stop() noexcept {
    buffered_inputs_.clear();
    context_ = nullptr;
}

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

    const auto key = std::make_pair(envelope.session_id, envelope.work_id);
    auto [found, inserted] = buffered_inputs_.try_emplace(key);
    static_cast<void>(inserted);
    if (found->second.size() + text.size() > kMaxTextBytes) {
        buffered_inputs_.erase(found);
        return runtime::MessageHandlingResult::kInvalidPayload;
    }
    found->second.append(text);
    if (!envelope.is_final) {
        return runtime::MessageHandlingResult::kHandled;
    }

    // 分片属于同一 work 的输入流；只在 final 到达时识别一次并回传原请求连接。
    const auto [intent, reply] = recognize_intent(found->second);
    buffered_inputs_.erase(found);
    v1::TextOutput output;
    output.set_request_message_id(envelope.message_id);
    output.set_intent(intent);
    output.set_text(std::string(reply));
    std::string payload;
    if (!output.SerializeToString(&payload) ||
        !output_handler_(envelope, "cockpit.text.output", std::move(payload))) {
        context_->logger().log(observability::Event{
            "dialogue", "text_output_not_delivered", envelope.trace_id,
            envelope.session_id, envelope.work_id, envelope.message_id, ""});
    }
    return runtime::MessageHandlingResult::kHandled;
}

}  // namespace cabinflow::agent
