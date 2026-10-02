#include <array>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "demo_nodes.hpp"

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

namespace {

cabinflow::protocol::Message make_message(
    const cabinflow::runtime::Clock& clock, std::string message_id,
    std::string session_id, std::string work_id, std::uint64_t sequence,
    std::string payload, bool is_final = false) {
    cabinflow::protocol::Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "demo-trace";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "text_source";
    message.envelope.target_node = "echo_processor";
    message.envelope.topic = "demo.text";
    message.envelope.sequence = sequence;
    message.envelope.created_monotonic_ns = clock.now_monotonic_ns();
    message.envelope.ttl_ms = 1000;
    message.envelope.is_final = is_final;
    message.payload = std::move(payload);
    return message;
}

}  // namespace

int main() {
    cabinflow::runtime::SteadyClock clock;
    cabinflow::observability::ConsoleLogger logger;
    cabinflow::transport::InMemoryTransport transport;
    cabinflow::runtime::Runtime runtime(transport, clock, logger);

    auto echo_processor = std::make_unique<cabinflow::demo::EchoProcessorNode>();
    auto* echo_processor_ptr = echo_processor.get();
    auto source = std::make_unique<cabinflow::demo::TextSourceNode>();
    auto* source_ptr = source.get();

    if (runtime.add_node(std::move(echo_processor)) !=
            cabinflow::runtime::RuntimeError::kNone ||
        runtime.add_node(std::move(source)) !=
            cabinflow::runtime::RuntimeError::kNone ||
        runtime.start() != cabinflow::runtime::RuntimeError::kNone) {
        std::cerr << "failed to start runtime demo\n";
        return 1;
    }

    const auto send = [source_ptr](cabinflow::protocol::Message message) {
        return source_ptr->publish(std::move(message)) ==
               cabinflow::transport::TransportError::kNone;
    };

    const auto first = make_message(clock, "demo-message", "driver-session",
                                    "demo-work", 0, "hello_cabinflow", true);
    if (!send(first) || !send(first) ||
        !send(make_message(clock, "after-final", "driver-session", "demo-work",
                           1, "must-reject"))) {
        std::cerr << "failed to publish initial demo cases\n";
        return 1;
    }

    std::array<bool, 2> concurrent_published{false, false};
    std::mutex start_mutex;
    std::condition_variable start_changed;
    int ready = 0;
    bool released = false;
    const auto start_together = [&] {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready;
        start_changed.notify_all();
        start_changed.wait(lock, [&] { return released; });
    };
    std::thread driver([&] {
        start_together();
        concurrent_published[0] = send(make_message(
            clock, "driver-concurrent", "driver-concurrent-session", "driver-work",
            0, "driver-text", true));
    });
    std::thread passenger([&] {
        start_together();
        concurrent_published[1] = send(make_message(
            clock, "passenger-concurrent", "passenger-session", "passenger-work",
            0, "passenger-text", true));
    });
    {
        // 两个发送线程都就绪后同时放行，避免用 sleep 碰运气制造并发。
        std::unique_lock<std::mutex> lock(start_mutex);
        start_changed.wait(lock, [&] { return ready == 2; });
        released = true;
    }
    start_changed.notify_all();
    driver.join();
    passenger.join();
    if (!concurrent_published[0] || !concurrent_published[1]) {
        std::cerr << "failed to publish concurrent demo cases\n";
        return 1;
    }

    if (!send(make_message(clock, "order-high", "driver-session", "order-work",
                           2, "first-arrival")) ||
        !send(make_message(clock, "order-low", "driver-session", "order-work",
                           1, "stale-arrival"))) {
        std::cerr << "failed to publish ordering demo cases\n";
        return 1;
    }

    auto expired = make_message(clock, "expired-message", "driver-session",
                                "deadline-work", 0, "too-late", true);
    const auto now = clock.now_monotonic_ns();
    if (now < 2'000'000ULL) {
        std::cerr << "monotonic clock has not reached the demo deadline offset\n";
        return 1;
    }
    expired.envelope.created_monotonic_ns = now - 2'000'000ULL;
    expired.envelope.ttl_ms = 1;
    if (!send(std::move(expired))) {
        std::cerr << "failed to publish expired demo case\n";
        return 1;
    }

    runtime.cancel_work("driver-session", "cancel-work");
    if (!send(make_message(clock, "cancelled-message", "driver-session",
                           "cancel-work", 0, "must-not-run")) ||
        !send(make_message(clock, "other-work-message", "driver-session",
                           "other-work", 0, "unaffected", true))) {
        std::cerr << "failed to publish cancellation demo cases\n";
        return 1;
    }

    runtime.stop();
    return echo_processor_ptr->completed_count() == 5 &&
                   runtime.metric_value("demo.source_published") == 10 &&
                   runtime.metric_value("demo.message_rejected") == 5
               ? 0
               : 1;
}
