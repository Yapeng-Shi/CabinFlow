#include "demo_nodes.hpp"

namespace cabinflow::demo {

std::string_view TextSourceNode::name() const noexcept { return "text_source"; }

runtime::RuntimeError TextSourceNode::start(runtime::NodeContext& context) {
    context_ = &context;
    return runtime::RuntimeError::kNone;
}

void TextSourceNode::stop() noexcept { context_ = nullptr; }

transport::TransportError TextSourceNode::publish(protocol::Message message) {
    if (context_ == nullptr) {
        return transport::TransportError::kClosed;
    }

    const auto result = context_->transport().publish(message);
    context_->metrics().increment(
        result == transport::TransportError::kNone ? "demo.source_published"
                                                   : "demo.source_rejected");
    context_->logger().log(
        {"text_source", "source_published", message.envelope.trace_id,
         message.envelope.session_id, message.envelope.work_id,
         message.envelope.message_id,
         {}, result == transport::TransportError::kNone ? "published"
                                                     : "transport_error"});
    return result;
}

}  // namespace cabinflow::demo
