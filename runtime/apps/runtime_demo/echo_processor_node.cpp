#include "demo_nodes.hpp"

#include <string>
#include <utility>

namespace cabinflow::demo {

std::string_view EchoProcessorNode::name() const noexcept {
    return "echo_processor";
}

runtime::RuntimeError EchoProcessorNode::start(runtime::NodeContext& context) {
    context_ = &context;
    auto result = context_->transport().subscribe(
        "demo.text", [this](const protocol::Message& message) {
            on_message(message);
        });
    if (!result) {
        context_ = nullptr;
        return runtime::RuntimeError::kNodeStartFailure;
    }

    subscription_ = std::move(result.subscription);
    return runtime::RuntimeError::kNone;
}

void EchoProcessorNode::stop() noexcept {
    subscription_.reset();
    context_ = nullptr;
}

std::size_t EchoProcessorNode::completed_count() const noexcept {
    return completed_count_.load();
}

void EchoProcessorNode::on_message(const protocol::Message& message) {
    if (context_ == nullptr) {
        return;
    }

    const auto decision = context_->ledger().observe(message.envelope);
    if (decision != runtime::DeliveryResult::kAccepted) {
        context_->metrics().increment("demo.message_rejected");
        context_->logger().log(
            {"echo_processor", "message_rejected", message.envelope.trace_id,
             message.envelope.session_id, message.envelope.work_id,
             message.envelope.message_id,
             {}, std::string(runtime::to_string(decision))});
        return;
    }

    ++completed_count_;
    context_->metrics().increment("demo.echo_completed");
    context_->logger().log(
        {"echo_processor", "echo_completed", message.envelope.trace_id,
         message.envelope.session_id, message.envelope.work_id,
         message.envelope.message_id, "echo:" + message.payload, "handled"});
}

}  // namespace cabinflow::demo
