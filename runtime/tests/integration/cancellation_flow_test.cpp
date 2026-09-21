#include <iostream>
#include <memory>
#include <string_view>

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

class CancellationProbeNode final : public cabinflow::runtime::Node {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "cancellation_probe";
    }

    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext& context) override {
        context_ = &context;
        token_ = context.cancellation_token("driver-session", "route-work");
        return cabinflow::runtime::RuntimeError::kNone;
    }

    void stop() noexcept override { context_ = nullptr; }

    [[nodiscard]] bool cancelled() const noexcept { return token_.cancelled(); }

    [[nodiscard]] cabinflow::runtime::DeliveryResult observe(
        const cabinflow::protocol::MessageEnvelope& envelope) const {
        return context_->ledger().observe(envelope);
    }

private:
    cabinflow::runtime::NodeContext* context_{nullptr};
    cabinflow::runtime::CancellationToken token_;
};

}  // namespace

int main() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    cabinflow::test::RecordingLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);

    auto probe = std::make_unique<CancellationProbeNode>();
    auto* probe_ptr = probe.get();
    if (!expect(runtime.add_node(std::move(probe)) ==
                    cabinflow::runtime::RuntimeError::kNone,
                "probe added") ||
        !expect(runtime.start() == cabinflow::runtime::RuntimeError::kNone,
                "runtime starts")) {
        return 1;
    }

    runtime.cancel_work("driver-session", "route-work");
    const auto cancelled_message = cabinflow::test::MessageBuilder()
                                       .with_topic("demo.text")
                                       .with_work_id("route-work")
                                       .build();
    if (!expect(probe_ptr->cancelled(), "in-flight token observes cancellation") ||
        !expect(probe_ptr->observe(cancelled_message.envelope) ==
                    cabinflow::runtime::DeliveryResult::kWorkCancelled,
                "later message rejected by ledger")) {
        return 1;
    }

    runtime.stop();
    return 0;
}
