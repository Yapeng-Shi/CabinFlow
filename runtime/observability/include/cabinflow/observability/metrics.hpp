#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace cabinflow::observability {

class Metrics {
public:
    virtual ~Metrics() = default;

    virtual void increment(std::string_view name) = 0;
};

class InMemoryMetrics final : public Metrics {
public:
    InMemoryMetrics();
    ~InMemoryMetrics() override;

    InMemoryMetrics(const InMemoryMetrics&) = delete;
    InMemoryMetrics& operator=(const InMemoryMetrics&) = delete;
    InMemoryMetrics(InMemoryMetrics&&) = delete;
    InMemoryMetrics& operator=(InMemoryMetrics&&) = delete;

    void increment(std::string_view name) override;

    [[nodiscard]] std::uint64_t value(std::string_view name) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace cabinflow::observability
