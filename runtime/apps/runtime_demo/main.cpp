#include <iostream>
#include <memory>

#include "demo_nodes.hpp"

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

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

    if (source_ptr->publish("hello_cabinflow") !=
        cabinflow::transport::TransportError::kNone) {
        std::cerr << "failed to publish demo text\n";
        return 1;
    }

    runtime.stop();
    return echo_processor_ptr->completed_count() == 1 ? 0 : 1;
}
