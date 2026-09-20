#include <cabinflow/observability/metrics.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cabinflow::observability {

struct InMemoryMetrics::State {
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::uint64_t> counters;
};

InMemoryMetrics::InMemoryMetrics() : state_(std::make_unique<State>()) {}

InMemoryMetrics::~InMemoryMetrics() = default;

void InMemoryMetrics::increment(std::string_view name) {
    // 指标名在写入时复制一次，调用方可安全传入临时 string_view。
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->counters[std::string(name)];
}

std::uint64_t InMemoryMetrics::value(std::string_view name) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto counter = state_->counters.find(std::string(name));
    return counter == state_->counters.end() ? 0 : counter->second;
}

}  // namespace cabinflow::observability
