#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/observability/metrics.hpp>
#include <cabinflow/runtime/cancellation.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/node.hpp>
#include <cabinflow/runtime/node_context.hpp>
#include <cabinflow/runtime/runtime_error.hpp>
#include <cabinflow/runtime/session_ledger.hpp>
#include <cabinflow/transport/transport.hpp>

namespace cabinflow::runtime {

class Runtime final {
public:
    Runtime(transport::ITransport& transport, Clock& clock,
            observability::Logger& logger);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] RuntimeError add_node(std::unique_ptr<Node> node);
    [[nodiscard]] RuntimeError start();
    void stop() noexcept;

    void cancel_session(std::string_view session_id);
    void cancel_work(std::string_view session_id, std::string_view work_id);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t metric_value(std::string_view name) const;

private:
    SessionLedger ledger_;
    CancellationRegistry cancellations_;
    observability::InMemoryMetrics metrics_;
    NodeContext context_;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::size_t started_node_count_{0};
    mutable std::mutex lifecycle_mutex_;
    bool running_{false};
};

}  // namespace cabinflow::runtime
