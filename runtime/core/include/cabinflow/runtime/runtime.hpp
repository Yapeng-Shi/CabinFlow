#pragma once

#include <cstdint>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/observability/metrics.hpp>
#include <cabinflow/runtime/cancellation.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/node.hpp>
#include <cabinflow/runtime/node_context.hpp>
#include <cabinflow/runtime/runtime_error.hpp>
#include <cabinflow/runtime/session_ledger.hpp>
#include <cabinflow/runtime/target_node.hpp>
#include <cabinflow/transport/transport.hpp>

namespace cabinflow::runtime {

class Runtime final {
public:
    enum class TargetReservationError {
        kNone,
        kNotRunning,
        kUnknownTarget,
        kQueueFull,
    };

    enum class TargetDeliveryFailure {
        kDeadlineExceeded,
        kSessionCancelled,
        kWorkCancelled,
        kUnsupportedTopic,
        kInvalidPayload,
    };

    using TargetDeliveryFailureHandler =
        std::function<void(TargetDeliveryFailure)>;

    class TargetReservation final {
    public:
        TargetReservation();
        ~TargetReservation();

        TargetReservation(const TargetReservation&) = delete;
        TargetReservation& operator=(const TargetReservation&) = delete;
        TargetReservation(TargetReservation&&) noexcept;
        TargetReservation& operator=(TargetReservation&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        struct State;
        friend class Runtime;
        explicit TargetReservation(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_;
    };

    struct TargetReservationResult {
        TargetReservation reservation;
        TargetReservationError error{TargetReservationError::kNone};

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == TargetReservationError::kNone;
        }
    };

    struct TargetAdmissionResult {
        DeliveryResult delivery_result{DeliveryResult::kAccepted};
        bool admitted{false};
    };

    struct TargetStats {
        std::size_t queue_depth{0};
        std::uint64_t queue_full_rejections{0};
        std::uint64_t handling_count{0};
        std::uint64_t handling_duration_ns_total{0};
    };

    Runtime(transport::ITransport& transport, Clock& clock,
            observability::Logger& logger);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] RuntimeError add_node(std::unique_ptr<Node> node);
    [[nodiscard]] RuntimeError add_target_node(
        std::unique_ptr<TargetNode> node, std::size_t queue_capacity);
    [[nodiscard]] RuntimeError start();
    // 一次性实例；当前验收覆盖外部同步启停，worker/节点回调直接 stop 的合同尚待确认。
    void stop() noexcept;

    // Gateway 先取得槽位，再由此函数在同一 Runtime 生命周期锁内完成 Ledger 准入与提交。
    [[nodiscard]] TargetReservationResult reserve_target(
        std::string_view target_node);
    [[nodiscard]] TargetAdmissionResult admit_reserved(
        TargetReservation&& reservation, protocol::Message message,
        TargetDeliveryFailureHandler failure_handler,
        std::function<void()> completion_handler = {});

    void cancel_session(std::string_view session_id);
    void cancel_work(std::string_view session_id, std::string_view work_id);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint64_t metric_value(std::string_view name) const;
    [[nodiscard]] std::optional<TargetStats> target_stats(
        std::string_view target_node) const;

private:
    enum class LifecycleState {
        kReady,
        kStarting,
        kRunning,
        kStopping,
        kStopped,
        kFailed,
    };

    struct TargetRegistration;
    struct TargetJob;

    void worker_loop(const std::shared_ptr<TargetRegistration>& target) noexcept;
    void release_started_nodes(bool record_stops) noexcept;

    Clock& clock_;
    SessionLedger ledger_;
    CancellationRegistry cancellations_;
    observability::InMemoryMetrics metrics_;
    NodeContext context_;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<std::shared_ptr<TargetRegistration>> targets_;
    std::unordered_map<std::string, std::shared_ptr<TargetRegistration>>
        targets_by_name_;
    std::vector<Node*> started_nodes_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    LifecycleState lifecycle_state_{LifecycleState::kReady};
};

}  // namespace cabinflow::runtime
