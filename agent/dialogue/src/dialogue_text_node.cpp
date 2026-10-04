#include <cabinflow/agent/dialogue_text_node.hpp>

#include <cstddef>
#include <cstdint>
#include <charconv>
#include <optional>
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
    if (text == "打开左前车窗") {
        return {v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_OPEN, "识别到打开左前车窗意图，未执行车控。"};
    }
    if (text == "关闭左前车窗") {
        return {v1::COCKPIT_INTENT_LEFT_FRONT_WINDOW_CLOSE, "识别到关闭左前车窗意图，未执行车控。"};
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

std::optional<v1::MusicCommand> recognize_music(std::string_view text) {
    v1::MusicCommand command;
    if (text.substr(0, std::string_view("搜索").size()) == "搜索") {
        auto keyword = text.substr(std::string_view("搜索").size());
        const auto first = keyword.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos)
            keyword = keyword.substr(first, keyword.find_last_not_of(" \t\r\n") - first + 1);
        else keyword = {};
        command.set_action(v1::MusicCommand::SEARCH);
        command.set_keyword(std::string(keyword));
    } else if (text == "播放音乐" || text == "继续播放") {
        command.set_action(v1::MusicCommand::PLAY);
    } else if (text == "暂停音乐") {
        command.set_action(v1::MusicCommand::PAUSE);
    } else if (text == "上一首") {
        command.set_action(v1::MusicCommand::PREVIOUS);
    } else if (text == "下一首") {
        command.set_action(v1::MusicCommand::NEXT);
    } else if (text.substr(0, std::string_view("播放第").size()) == "播放第" &&
               text.size() > std::string_view("播放第首").size() &&
               text.substr(text.size() - std::string_view("首").size()) == "首") {
        const auto ordinal = text.substr(std::string_view("播放第").size(),
            text.size() - std::string_view("播放第首").size());
        std::uint32_t index = 0;
        const auto parsed = std::from_chars(ordinal.data(), ordinal.data() + ordinal.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != ordinal.data() + ordinal.size()) {
            constexpr std::string_view numbers[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
            index = 0;
            for (std::uint32_t i = 0; i < 10; ++i) if (ordinal == numbers[i]) index = i + 1;
        }
        // 格式已命中但编号非法时保留 SELECT(0)，下游显式拒绝，不交给 LLM 猜执行。
        command.set_action(v1::MusicCommand::SELECT);
        command.set_result_index(index);
    } else return std::nullopt;
    return command;
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
    const auto music = recognize_music(found->second);
    const auto [intent, reply] = recognize_intent(found->second);
    buffered_inputs_.erase(found);
    v1::TextOutput output;
    output.set_request_message_id(envelope.message_id);
    output.set_intent(intent);
    output.set_text(std::string(reply));
    if (music) {
        output.set_intent(v1::COCKPIT_INTENT_MUSIC);
        *output.mutable_music_command() = *music;
        output.set_text("音乐指令已识别，尚未执行播放器操作。");
    }
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
