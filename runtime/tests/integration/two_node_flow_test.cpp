#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cabinflow/runtime/node.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "fake_clock.hpp"
#include "message_builder.hpp"
#include "recording_logger.hpp"

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

class TextSourceNode final : public cabinflow::runtime::Node {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "text_source";
    }

    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext& context) override {
        context_ = &context;
        return cabinflow::runtime::RuntimeError::kNone;
    }

    void stop() noexcept override { context_ = nullptr; }

    [[nodiscard]] cabinflow::transport::TransportError publish(
        std::string_view text) {
        if (context_ == nullptr) {
            return cabinflow::transport::TransportError::kClosed;
        }

        auto message = cabinflow::test::MessageBuilder()
                           .with_topic("demo.text")
                           .with_payload(text)
                           .with_created_monotonic_ns(
                               context_->clock().now_monotonic_ns())
                           .build();
        return context_->transport().publish(message);
    }

private:
    cabinflow::runtime::NodeContext* context_{nullptr};
};

class EchoProcessorNode final : public cabinflow::runtime::Node {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "echo_processor";
    }

    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext& context) override {
        context_ = &context;
        auto result = context_->transport().subscribe(
            "demo.text", [this](const cabinflow::protocol::Message& message) {
                on_message(message);
            });
        if (!result) {
            context_ = nullptr;
            return cabinflow::runtime::RuntimeError::kNodeStartFailure;
        }

        subscription_ = std::move(result.subscription);
        return cabinflow::runtime::RuntimeError::kNone;
    }

    void stop() noexcept override {
        subscription_.reset();
        context_ = nullptr;
    }

    [[nodiscard]] std::size_t completed_count() const noexcept {
        return completed_count_;
    }

private:
    void on_message(const cabinflow::protocol::Message& message) {
        const auto decision = context_->ledger().observe(message.envelope);
        if (decision != cabinflow::runtime::DeliveryResult::kAccepted) {
            return;
        }

        ++completed_count_;
        context_->logger().log(
            {"echo_processor", "echo_completed", message.envelope.trace_id,
             message.envelope.session_id, message.envelope.work_id,
             message.envelope.message_id, "echo:" + message.payload});
    }

    cabinflow::runtime::NodeContext* context_{nullptr};
    std::unique_ptr<cabinflow::transport::Subscription> subscription_;
    std::size_t completed_count_{0};
};

}  // namespace

int main() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    cabinflow::test::RecordingLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);

    auto processor = std::make_unique<EchoProcessorNode>();
    auto* processor_ptr = processor.get();
    auto source = std::make_unique<TextSourceNode>();
    auto* source_ptr = source.get();

    if (!expect(runtime.add_node(std::move(processor)) ==
                    cabinflow::runtime::RuntimeError::kNone,
                "processor added") ||
        !expect(runtime.add_node(std::move(source)) ==
                    cabinflow::runtime::RuntimeError::kNone,
                "source added") ||
        !expect(runtime.start() == cabinflow::runtime::RuntimeError::kNone,
                "runtime starts") ||
        !expect(source_ptr->publish("hello_cabinflow") ==
                    cabinflow::transport::TransportError::kNone,
                "source publishes") ||
        !expect(processor_ptr->completed_count() == 1,
                "processor receives one message")) {
        return 1;
    }

    const auto events = logger.snapshot();
    if (!expect(events.size() == 1, "one structured event") ||
        !expect(events.front().name == "echo_completed", "event name") ||
        !expect(events.front().trace_id == "test-trace", "trace propagated") ||
        !expect(events.front().session_id == "driver-session",
                "session propagated") ||
        !expect(events.front().work_id == "test-work", "work propagated") ||
        !expect(events.front().message_id == "test-message",
                "message id propagated") ||
        !expect(events.front().detail == "echo:hello_cabinflow",
                "payload propagated")) {
        return 1;
    }

    runtime.stop();
    return 0;
}
