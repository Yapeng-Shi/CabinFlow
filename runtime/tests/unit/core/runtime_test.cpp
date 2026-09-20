#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message_envelope.hpp>
#include <cabinflow/runtime/node.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "fake_clock.hpp"

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

class NullLogger final : public cabinflow::observability::Logger {
public:
    void log(const cabinflow::observability::Event&) override {}
};

class RecordingNode final : public cabinflow::runtime::Node {
public:
    RecordingNode(std::string name, std::vector<std::string>& events,
                  cabinflow::runtime::RuntimeError start_result)
        : name_(std::move(name)),
          events_(events),
          start_result_(start_result) {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext& context) override {
        context_ = &context;
        token_ = context.cancellation_token("driver-session", "route-work");
        events_.push_back(name_ + ":start");
        return start_result_;
    }

    void stop() noexcept override { events_.push_back(name_ + ":stop"); }

    [[nodiscard]] const cabinflow::runtime::CancellationToken& token() const {
        return token_;
    }

    [[nodiscard]] cabinflow::runtime::DeliveryResult observe(
        const cabinflow::protocol::MessageEnvelope& envelope) const {
        return context_->ledger().observe(envelope);
    }

private:
    std::string name_;
    std::vector<std::string>& events_;
    cabinflow::runtime::RuntimeError start_result_;
    cabinflow::runtime::CancellationToken token_;
    cabinflow::runtime::NodeContext* context_{nullptr};
};

cabinflow::protocol::MessageEnvelope make_envelope() {
    cabinflow::protocol::MessageEnvelope envelope;
    envelope.message_id = "cancelled-message";
    envelope.trace_id = "cancelled-trace";
    envelope.session_id = "driver-session";
    envelope.work_id = "route-work";
    envelope.source_node = "source";
    envelope.target_node = "worker";
    envelope.topic = "demo.text";
    envelope.created_monotonic_ns = 1'000'000;
    envelope.ttl_ms = 100;
    return envelope;
}

}  // namespace

int main() {
    using cabinflow::runtime::Runtime;
    using cabinflow::runtime::RuntimeError;

    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;

    std::vector<std::string> failed_events;
    Runtime failed_runtime(transport, clock, logger);
    if (!expect(failed_runtime.add_node(nullptr) == RuntimeError::kInvalidNode,
                "null node rejected") ||
        !expect(failed_runtime.add_node(std::make_unique<RecordingNode>(
                    "first", failed_events, RuntimeError::kNone)) ==
                    RuntimeError::kNone,
                "first node added") ||
        !expect(failed_runtime.add_node(std::make_unique<RecordingNode>(
                    "rejecting", failed_events,
                    RuntimeError::kNodeStartFailure)) == RuntimeError::kNone,
                "rejecting node added") ||
        !expect(failed_runtime.start() == RuntimeError::kNodeStartFailure,
                "startup failure propagated") ||
        !expect(!failed_runtime.running(), "failed runtime not running") ||
        !expect(failed_events == std::vector<std::string>{
                                    "first:start", "rejecting:start", "first:stop"},
                "started nodes stopped in reverse order")) {
        return 1;
    }

    std::vector<std::string> events;
    Runtime runtime(transport, clock, logger);
    auto node = std::make_unique<RecordingNode>("worker", events,
                                                RuntimeError::kNone);
    auto* node_ptr = node.get();
    if (!expect(runtime.add_node(std::move(node)) == RuntimeError::kNone,
                "node added") ||
        !expect(runtime.start() == RuntimeError::kNone, "runtime starts") ||
        !expect(runtime.running(), "runtime running") ||
        !expect(node_ptr->token().valid(), "node receives work token") ||
        !expect(runtime.metric_value("runtime.node_started") == 1,
                "node startup metric") ||
        !expect(runtime.add_node(std::make_unique<RecordingNode>(
                    "late", events, RuntimeError::kNone)) ==
                    RuntimeError::kAlreadyStarted,
                "node addition after start rejected")) {
        return 1;
    }

    runtime.cancel_work("driver-session", "route-work");
    if (!expect(node_ptr->token().cancelled(),
                "runtime cancellation reaches in-flight token") ||
        !expect(node_ptr->observe(make_envelope()) ==
                    cabinflow::runtime::DeliveryResult::kWorkCancelled,
                "runtime cancellation rejects subsequent delivery") ||
        !expect(runtime.metric_value("runtime.work_cancelled") == 1,
                "work cancellation metric")) {
        return 1;
    }

    runtime.stop();
    if (!expect(!runtime.running(), "runtime stopped") ||
        !expect(runtime.metric_value("runtime.node_stopped") == 1,
                "node shutdown metric") ||
        !expect(events == std::vector<std::string>{"worker:start", "worker:stop"},
                "normal node lifecycle")) {
        return 1;
    }

    return 0;
}
