#pragma once

#include <string_view>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/observability/metrics.hpp>
#include <cabinflow/runtime/cancellation.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/session_ledger.hpp>
#include <cabinflow/transport/transport.hpp>

namespace cabinflow::runtime {

class NodeContext final {
public:
    NodeContext(transport::ITransport& transport, Clock& clock,
                SessionLedger& ledger, CancellationRegistry& cancellations,
                observability::Logger& logger,
                observability::Metrics& metrics) noexcept
        : transport_(transport),
          clock_(clock),
          ledger_(ledger),
          cancellations_(cancellations),
          logger_(logger),
          metrics_(metrics) {}

    [[nodiscard]] transport::ITransport& transport() noexcept {
        return transport_;
    }
    [[nodiscard]] Clock& clock() noexcept { return clock_; }
    [[nodiscard]] SessionLedger& ledger() noexcept { return ledger_; }
    [[nodiscard]] observability::Logger& logger() noexcept { return logger_; }
    [[nodiscard]] observability::Metrics& metrics() noexcept { return metrics_; }

    [[nodiscard]] CancellationToken cancellation_token(
        std::string_view session_id, std::string_view work_id) const {
        return cancellations_.token_for(session_id, work_id);
    }

private:
    transport::ITransport& transport_;
    Clock& clock_;
    SessionLedger& ledger_;
    CancellationRegistry& cancellations_;
    observability::Logger& logger_;
    observability::Metrics& metrics_;
};

}  // namespace cabinflow::runtime
