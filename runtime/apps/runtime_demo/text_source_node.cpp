#include "demo_nodes.hpp"

#include <string>

namespace cabinflow::demo {

std::string_view TextSourceNode::name() const noexcept { return "text_source"; }

runtime::RuntimeError TextSourceNode::start(runtime::NodeContext& context) {
    context_ = &context;
    return runtime::RuntimeError::kNone;
}

void TextSourceNode::stop() noexcept { context_ = nullptr; }

transport::TransportError TextSourceNode::publish(std::string_view text) {
    if (context_ == nullptr) {
        return transport::TransportError::kClosed;
    }

    protocol::Message message;
    message.envelope.message_id = "demo-message";
    message.envelope.trace_id = "demo-trace";
    message.envelope.session_id = "driver-session";
    message.envelope.work_id = "demo-work";
    message.envelope.source_node = "text_source";
    message.envelope.target_node = "echo_processor";
    message.envelope.topic = "demo.text";
    message.envelope.created_monotonic_ns =
        context_->clock().now_monotonic_ns();
    message.envelope.ttl_ms = 1000;
    message.envelope.is_final = true;
    message.payload = text;

    const auto result = context_->transport().publish(message);
    context_->metrics().increment(
        result == transport::TransportError::kNone ? "demo.source_published"
                                                   : "demo.source_rejected");
    context_->logger().log(
        {"text_source", "source_published", message.envelope.trace_id,
         message.envelope.session_id, message.envelope.work_id,
         message.envelope.message_id,
         result == transport::TransportError::kNone ? "accepted" : "rejected"});
    return result;
}

}  // namespace cabinflow::demo
