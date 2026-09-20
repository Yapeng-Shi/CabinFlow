#include <iostream>
#include <string_view>

#include <cabinflow/observability/metrics.hpp>

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

}  // namespace

int main() {
    cabinflow::observability::InMemoryMetrics metrics;
    if (!expect(metrics.value("runtime.node_started") == 0,
                "missing metric is zero")) {
        return 1;
    }

    metrics.increment("runtime.node_started");
    metrics.increment("runtime.node_started");
    metrics.increment("runtime.node_stopped");
    if (!expect(metrics.value("runtime.node_started") == 2,
                "counter increments") ||
        !expect(metrics.value("runtime.node_stopped") == 1,
                "counter isolation")) {
        return 1;
    }

    return 0;
}
