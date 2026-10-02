#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/target_node.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "fake_clock.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

class NullLogger final : public cabinflow::observability::Logger {
public:
    void log(const cabinflow::observability::Event&) override {}
};

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool block{false};
    bool entered{false};
    bool released{false};
    std::size_t handled{0};
    std::vector<std::string> message_ids;

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, 1s, [this] { return entered; }),
                "target did not begin handling");
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }

    void wait_until_handled(std::size_t expected) {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, 1s, [this, expected] {
                    return handled >= expected;
                }),
                "target did not handle expected message count");
    }
};

class ScriptedTarget final : public cabinflow::runtime::TargetNode {
public:
    ScriptedTarget(std::string name, Gate& gate,
                   cabinflow::runtime::MessageHandlingResult result)
        : name_(std::move(name)), gate_(gate), result_(result) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }

    void stop() noexcept override {}

    [[nodiscard]] cabinflow::runtime::MessageHandlingResult on_message(
        const cabinflow::protocol::Message& message) noexcept override {
        std::unique_lock<std::mutex> lock(gate_.mutex);
        gate_.entered = true;
        ++gate_.handled;
        gate_.message_ids.push_back(message.envelope.message_id);
        gate_.changed.notify_all();
        while (gate_.block && !gate_.released) {
            gate_.changed.wait(lock);
        }
        return result_;
    }

private:
    std::string name_;
    Gate& gate_;
    cabinflow::runtime::MessageHandlingResult result_;
};

class EmptyTarget final : public cabinflow::runtime::TargetNode {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return {}; }
    [[nodiscard]] cabinflow::runtime::RuntimeError start(
        cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }
    void stop() noexcept override {}
    [[nodiscard]] cabinflow::runtime::MessageHandlingResult on_message(
        const cabinflow::protocol::Message&) noexcept override {
        return cabinflow::runtime::MessageHandlingResult::kHandled;
    }
};

cabinflow::protocol::Message make_message(std::string message_id,
                                          std::string target,
                                          std::uint64_t sequence,
                                          std::uint64_t created_ns = 1'000'000,
                                          std::uint32_t ttl_ms = 1'000) {
    cabinflow::protocol::Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace";
    message.envelope.session_id = "session";
    message.envelope.work_id = "work";
    message.envelope.source_node = "gateway";
    message.envelope.target_node = std::move(target);
    message.envelope.topic = "target." + message.envelope.target_node;
    message.envelope.sequence = sequence;
    message.envelope.created_monotonic_ns = created_ns;
    message.envelope.ttl_ms = ttl_ms;
    message.payload = "typed payload";
    return message;
}

void test_registration_and_reservation_invariants() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);
    Gate gate;
    gate.block = true;

    require(runtime.add_target_node(nullptr, 1) ==
                cabinflow::runtime::RuntimeError::kInvalidNode,
            "null target is rejected");
    require(runtime.add_target_node(std::make_unique<EmptyTarget>(), 1) ==
                cabinflow::runtime::RuntimeError::kEmptyTargetNodeName,
            "empty target name is rejected");
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "dialogue.primary", gate,
                cabinflow::runtime::MessageHandlingResult::kHandled), 0) ==
                cabinflow::runtime::RuntimeError::kInvalidTargetQueueCapacity,
            "zero target capacity is rejected");
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "dialogue.primary", gate,
                cabinflow::runtime::MessageHandlingResult::kHandled), 1) ==
                cabinflow::runtime::RuntimeError::kNone,
            "target registration succeeds");
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "dialogue.primary", gate,
                cabinflow::runtime::MessageHandlingResult::kHandled), 1) ==
                cabinflow::runtime::RuntimeError::kDuplicateTargetNodeName,
            "duplicate target name is rejected");
    require(runtime.start() == cabinflow::runtime::RuntimeError::kNone,
            "runtime starts target worker");
    require(!runtime.target_stats("missing.target").has_value(),
            "unknown target has no observation snapshot");

    auto active = runtime.reserve_target("dialogue.primary");
    require(runtime.admit_reserved(std::move(active.reservation),
                                   make_message("active-message", "dialogue.primary", 0), {})
                .admitted,
            "active message is admitted");
    gate.wait_until_entered();
    clock.advance(250);

    auto queued = runtime.reserve_target("dialogue.primary");
    require(runtime.admit_reserved(std::move(queued.reservation),
                                   make_message("queued-message", "dialogue.primary", 1), {})
                .admitted,
            "existing queued message is admitted");
    const auto full = runtime.reserve_target("dialogue.primary");
    require(full.error == cabinflow::runtime::Runtime::TargetReservationError::kQueueFull,
            "full target queue rejects without mutating ledger or evicting queued work");
    const auto blocked_stats = runtime.target_stats("dialogue.primary");
    require(blocked_stats.has_value() && blocked_stats->queue_depth == 1 &&
                blocked_stats->queue_full_rejections == 1 &&
                blocked_stats->handling_count == 0,
            "slow target exposes queued depth and one rejected reservation");

    auto final_message = make_message("retry-after-full", "dialogue.primary", 2);
    final_message.envelope.is_final = true;

    gate.release();
    gate.wait_until_handled(2);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        require(gate.message_ids ==
                    std::vector<std::string>{"active-message", "queued-message"},
                "queue-full rejection preserves the previously queued item");
    }

    auto retry = runtime.reserve_target("dialogue.primary");
    require(static_cast<bool>(retry), "released reservation restores capacity");
    const auto admitted = runtime.admit_reserved(std::move(retry.reservation),
                                                  final_message, {});
    require(admitted.admitted, "final message rejected by full queue can retry safely");
    gate.wait_until_handled(3);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        require(gate.message_ids.back() == "retry-after-full",
                "same final message reaches the target after capacity frees");
    }
    runtime.stop();
    const auto completed_stats = runtime.target_stats("dialogue.primary");
    require(completed_stats.has_value() && completed_stats->queue_depth == 0 &&
                completed_stats->queue_full_rejections == 1 &&
                completed_stats->handling_count == 3 &&
                completed_stats->handling_duration_ns_total == 250,
            "target snapshot counts completed handlers and injected-clock duration");
}

void test_worker_failure_and_deadline_rules() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);
    Gate unsupported_gate;
    Gate invalid_gate;
    Gate delayed_gate;
    delayed_gate.block = true;
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "unsupported", unsupported_gate,
                cabinflow::runtime::MessageHandlingResult::kUnsupportedTopic), 2) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "invalid", invalid_gate,
                cabinflow::runtime::MessageHandlingResult::kInvalidPayload), 2) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "delayed", delayed_gate,
                cabinflow::runtime::MessageHandlingResult::kHandled), 2) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.start() == cabinflow::runtime::RuntimeError::kNone,
            "failure targets start");

    std::mutex failures_mutex;
    std::condition_variable failures_changed;
    std::vector<cabinflow::runtime::Runtime::TargetDeliveryFailure> failures;
    const auto record_failure = [&failures_mutex, &failures_changed, &failures](
                                    cabinflow::runtime::Runtime::TargetDeliveryFailure failure) {
        std::lock_guard<std::mutex> lock(failures_mutex);
        failures.push_back(failure);
        failures_changed.notify_all();
    };

    for (const auto& target : {"unsupported", "invalid"}) {
        auto reservation = runtime.reserve_target(target);
        require(static_cast<bool>(reservation), "target queue reservation succeeds");
        const auto admitted = runtime.admit_reserved(
            std::move(reservation.reservation),
            make_message(std::string("failure-") + target, target, 0), record_failure);
        require(admitted.admitted, "failure message is admitted once");
    }

    {
        std::unique_lock<std::mutex> lock(failures_mutex);
        require(failures_changed.wait_for(lock, 1s, [&failures] {
                    return failures.size() == 2;
                }),
                "target result failures are reported before deadline test");
    }

    auto first = runtime.reserve_target("delayed");
    auto second = runtime.reserve_target("delayed");
    require(static_cast<bool>(first) && static_cast<bool>(second),
            "queued deadline test reserves both slots");
    require(runtime.admit_reserved(std::move(first.reservation),
                                   make_message("slow", "delayed", 0), record_failure).admitted,
            "blocking item is admitted");
    delayed_gate.wait_until_entered();
    require(runtime.admit_reserved(std::move(second.reservation),
                                   make_message("expires-in-queue", "delayed", 1,
                                                1'000'000, 1), record_failure).admitted,
            "queued item is admitted before its deadline");
    clock.advance(1'000'000);
    delayed_gate.release();

    std::unique_lock<std::mutex> lock(failures_mutex);
    require(failures_changed.wait_for(lock, 1s, [&failures] {
                return failures.size() == 3;
            }),
            "target failures are reported");
    require(failures[0] ==
                cabinflow::runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic ||
                failures[1] ==
                cabinflow::runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic,
            "unsupported topic maps through the Runtime worker");
    require(failures[0] ==
                cabinflow::runtime::Runtime::TargetDeliveryFailure::kInvalidPayload ||
                failures[1] ==
                cabinflow::runtime::Runtime::TargetDeliveryFailure::kInvalidPayload,
            "invalid payload maps through the Runtime worker");
    require(failures[2] ==
                cabinflow::runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded,
            "expired queued message never reaches the target handler");
    lock.unlock();
    runtime.stop();
}

void test_target_workers_are_isolated() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);
    Gate slow_gate;
    slow_gate.block = true;
    Gate fast_gate;
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "slow", slow_gate, cabinflow::runtime::MessageHandlingResult::kHandled), 1) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "fast", fast_gate, cabinflow::runtime::MessageHandlingResult::kHandled), 1) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.start() == cabinflow::runtime::RuntimeError::kNone,
            "isolated targets start");

    auto slow = runtime.reserve_target("slow");
    require(runtime.admit_reserved(std::move(slow.reservation),
                                   make_message("slow-active", "slow", 0), {}).admitted,
            "slow target accepts its active message");
    slow_gate.wait_until_entered();

    auto slow_queued = runtime.reserve_target("slow");
    require(runtime.admit_reserved(std::move(slow_queued.reservation),
                                   make_message("slow-queued", "slow", 1), {}).admitted,
            "slow target queues one additional message");
    require(runtime.reserve_target("slow").error ==
                cabinflow::runtime::Runtime::TargetReservationError::kQueueFull,
            "slow target is full");

    auto fast = runtime.reserve_target("fast");
    require(runtime.admit_reserved(std::move(fast.reservation),
                                   make_message("fast", "fast", 0), {}).admitted,
            "full slow queue does not block admission to fast target");
    fast_gate.wait_until_handled(1);
    slow_gate.release();
    slow_gate.wait_until_handled(2);
    runtime.stop();
}

void test_queued_cancellation(bool cancel_session) {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);
    Gate gate;
    gate.block = true;
    require(runtime.add_target_node(std::make_unique<ScriptedTarget>(
                "cancel-target", gate,
                cabinflow::runtime::MessageHandlingResult::kHandled), 1) ==
                cabinflow::runtime::RuntimeError::kNone &&
                runtime.start() == cabinflow::runtime::RuntimeError::kNone,
            "cancellation target starts");

    auto active = runtime.reserve_target("cancel-target");
    require(runtime.admit_reserved(
                std::move(active.reservation),
                make_message("already-running", "cancel-target", 0), {}).admitted,
            "first message begins before cancellation");
    gate.wait_until_entered();

    std::mutex failure_mutex;
    std::condition_variable failure_changed;
    std::vector<cabinflow::runtime::Runtime::TargetDeliveryFailure> failures;
    auto queued = runtime.reserve_target("cancel-target");
    require(runtime.admit_reserved(
                std::move(queued.reservation),
                make_message("not-started", "cancel-target", 1),
                [&](cabinflow::runtime::Runtime::TargetDeliveryFailure failure) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    failures.push_back(failure);
                    failure_changed.notify_all();
                }).admitted,
            "second message waits in the target queue");

    if (cancel_session) {
        runtime.cancel_session("session");
    } else {
        runtime.cancel_work("session", "work");
    }
    gate.release();

    {
        std::unique_lock<std::mutex> lock(failure_mutex);
        require(failure_changed.wait_for(lock, 1s, [&] {
                    return failures.size() == 1;
                }),
                "queued cancellation reports a typed failure");
        require(failures.front() ==
                    (cancel_session
                         ? cabinflow::runtime::Runtime::TargetDeliveryFailure::
                               kSessionCancelled
                         : cabinflow::runtime::Runtime::TargetDeliveryFailure::
                               kWorkCancelled),
                "cancellation scope is preserved for the queued message");
    }
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        require(gate.message_ids == std::vector<std::string>{"already-running"},
                "queued cancelled message never enters the target handler");
    }
    runtime.stop();
}

}  // namespace

int main() {
    try {
        test_registration_and_reservation_invariants();
        test_worker_failure_and_deadline_rules();
        test_target_workers_are_isolated();
        test_queued_cancellation(false);
        test_queued_cancellation(true);
        std::cout << "target runtime contract test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "target runtime contract test failed: " << error.what() << '\n';
        return 1;
    }
}
