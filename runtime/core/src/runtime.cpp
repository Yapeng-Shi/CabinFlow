#include <cabinflow/runtime/runtime.hpp>

#include <atomic>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <cabinflow/runtime/bounded_queue.hpp>

namespace cabinflow::runtime {

struct Runtime::TargetJob {
    protocol::Message message;
    TargetDeliveryFailureHandler failure_handler;
    std::function<void()> completion_handler;
};

struct Runtime::TargetRegistration {
    TargetRegistration(std::unique_ptr<TargetNode> target_node,
                       std::string target_name,
                       std::size_t queue_capacity)
        : name(std::move(target_name)),
          node(std::move(target_node)),
          queue(queue_capacity) {}

    std::string name;
    std::unique_ptr<TargetNode> node;
    BoundedQueue<TargetJob> queue;
    std::thread worker;
    std::atomic<std::uint64_t> queue_full_rejections{0};
    std::atomic<std::uint64_t> handling_count{0};
    std::atomic<std::uint64_t> handling_duration_ns_total{0};
};

struct Runtime::TargetReservation::State {
    State(std::shared_ptr<TargetRegistration> target_registration,
          BoundedQueue<TargetJob>::Reservation queue_reservation)
        : target(std::move(target_registration)),
          reservation(std::move(queue_reservation)) {}

    std::shared_ptr<TargetRegistration> target;
    BoundedQueue<TargetJob>::Reservation reservation;
};

Runtime::TargetReservation::TargetReservation() = default;
Runtime::TargetReservation::~TargetReservation() = default;
Runtime::TargetReservation::TargetReservation(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}
Runtime::TargetReservation::TargetReservation(TargetReservation&&) noexcept = default;
Runtime::TargetReservation& Runtime::TargetReservation::operator=(
    TargetReservation&&) noexcept = default;

Runtime::TargetReservation::operator bool() const noexcept {
    return state_ != nullptr && state_->reservation.active();
}

namespace {

constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;

[[nodiscard]] bool deadline_expired(const protocol::MessageEnvelope& envelope,
                                    const Clock& clock) noexcept {
    const auto now = clock.now_monotonic_ns();
    if (now < envelope.created_monotonic_ns || envelope.ttl_ms == 0U) {
        return true;
    }

    const auto ttl_ns = static_cast<std::uint64_t>(envelope.ttl_ms) *
                        kNanosecondsPerMillisecond;
    return now - envelope.created_monotonic_ns >= ttl_ns;
}

}  // namespace

Runtime::Runtime(transport::ITransport& transport, Clock& clock,
                 observability::Logger& logger)
    : clock_(clock),
      ledger_(clock),
      context_(transport, clock, ledger_, cancellations_, logger, metrics_) {}

Runtime::~Runtime() { stop(); }

RuntimeError Runtime::add_node(std::unique_ptr<Node> node) {
    if (node == nullptr) {
        return RuntimeError::kInvalidNode;
    }
    if (dynamic_cast<TargetNode*>(node.get()) != nullptr) {
        return RuntimeError::kTargetNodeRequiresTargetRegistration;
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (lifecycle_state_ != LifecycleState::kReady) {
        return lifecycle_state_ == LifecycleState::kRunning ||
                       lifecycle_state_ == LifecycleState::kStarting
                   ? RuntimeError::kAlreadyStarted
                   : RuntimeError::kLifecycleEnded;
    }
    nodes_.push_back(std::move(node));
    return RuntimeError::kNone;
}

RuntimeError Runtime::add_target_node(std::unique_ptr<TargetNode> node,
                                      std::size_t queue_capacity) {
    if (node == nullptr) {
        return RuntimeError::kInvalidNode;
    }
    if (queue_capacity == 0U) {
        return RuntimeError::kInvalidTargetQueueCapacity;
    }

    const std::string name(node->name());
    if (name.empty()) {
        return RuntimeError::kEmptyTargetNodeName;
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (lifecycle_state_ != LifecycleState::kReady) {
        return lifecycle_state_ == LifecycleState::kRunning ||
                       lifecycle_state_ == LifecycleState::kStarting
                   ? RuntimeError::kAlreadyStarted
                   : RuntimeError::kLifecycleEnded;
    }
    if (targets_by_name_.count(name) != 0U) {
        return RuntimeError::kDuplicateTargetNodeName;
    }

    auto target = std::make_shared<TargetRegistration>(std::move(node), name,
                                                        queue_capacity);
    targets_by_name_.emplace(name, target);
    targets_.push_back(std::move(target));
    return RuntimeError::kNone;
}

RuntimeError Runtime::start() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (lifecycle_state_ != LifecycleState::kReady) {
            return lifecycle_state_ == LifecycleState::kRunning ||
                           lifecycle_state_ == LifecycleState::kStarting
                       ? RuntimeError::kAlreadyStarted
                       : RuntimeError::kLifecycleEnded;
        }
        // Starting 冻结注册与数据准入；节点回调在锁外运行，允许查询 Runtime。
        lifecycle_state_ = LifecycleState::kStarting;
    }

    const auto start_node = [this](Node& node) {
        if (node.start(context_) != RuntimeError::kNone) {
            metrics_.increment("runtime.node_start_failure");
            return false;
        }
        started_nodes_.push_back(&node);
        metrics_.increment("runtime.node_started");
        return true;
    };

    auto result = RuntimeError::kNone;
    try {
        // 先分配启动记录，避免 node.start 成功后因 vector 扩容失败而丢失清理所有权。
        started_nodes_.reserve(nodes_.size() + targets_.size());
        for (const auto& node : nodes_) {
            if (!start_node(*node)) {
                result = RuntimeError::kNodeStartFailure;
                break;
            }
        }
        if (result == RuntimeError::kNone) {
            for (const auto& target : targets_) {
                if (!start_node(*target->node)) {
                    result = RuntimeError::kNodeStartFailure;
                    break;
                }
            }
        }
        if (result == RuntimeError::kNone) {
            try {
                for (const auto& target : targets_) {
                    target->worker =
                        std::thread([this, target] { worker_loop(target); });
                }
            } catch (const std::system_error&) {
                result = RuntimeError::kTargetWorkerStartFailure;
            }
        }
    } catch (...) {
        // 只清理后原样传播异常，不吞错、不重试；Failed 必须在回滚完成后发布。
        release_started_nodes(false);
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            lifecycle_state_ = LifecycleState::kFailed;
        }
        lifecycle_changed_.notify_all();
        throw;
    }

    if (result != RuntimeError::kNone) {
        release_started_nodes(false);
    }
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lifecycle_state_ = result == RuntimeError::kNone
                               ? LifecycleState::kRunning
                               : LifecycleState::kFailed;
    }
    lifecycle_changed_.notify_all();
    return result;
}

void Runtime::stop() noexcept {
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        // 外部并发 stop 等待唯一清理者完成，不能在 Stopping 提前返回。
        if (lifecycle_state_ == LifecycleState::kStarting ||
            lifecycle_state_ == LifecycleState::kStopping) {
            metrics_.increment("runtime.stop_waits");
            lifecycle_changed_.wait(lock, [this] {
                return lifecycle_state_ != LifecycleState::kStarting &&
                       lifecycle_state_ != LifecycleState::kStopping;
            });
        }
        if (lifecycle_state_ == LifecycleState::kStopped ||
            lifecycle_state_ == LifecycleState::kFailed) {
            return;
        }
        lifecycle_state_ = LifecycleState::kStopping;
    }

    // 状态已切断准入；不能持生命周期锁 join，否则 worker 回调 Runtime 会形成锁环。
    release_started_nodes(true);
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lifecycle_state_ = LifecycleState::kStopped;
    }
    lifecycle_changed_.notify_all();
}

void Runtime::release_started_nodes(bool record_stops) noexcept {
    for (const auto& target : targets_) {
        target->queue.close_and_discard();
    }
    for (const auto& target : targets_) {
        if (target->worker.joinable()) {
            target->worker.join();
        }
    }
    while (!started_nodes_.empty()) {
        started_nodes_.back()->stop();
        started_nodes_.pop_back();
        if (record_stops) {
            metrics_.increment("runtime.node_stopped");
        }
    }
}

Runtime::TargetReservationResult Runtime::reserve_target(
    std::string_view target_node) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (lifecycle_state_ != LifecycleState::kRunning) {
        return {TargetReservation{}, TargetReservationError::kNotRunning};
    }

    const auto found = targets_by_name_.find(std::string(target_node));
    if (found == targets_by_name_.end()) {
        return {TargetReservation{}, TargetReservationError::kUnknownTarget};
    }

    auto reserved = found->second->queue.try_reserve();
    if (!reserved) {
        if (reserved.error == QueueReserveError::kFull) {
            found->second->queue_full_rejections.fetch_add(
                1, std::memory_order_relaxed);
        }
        return {TargetReservation{}, TargetReservationError::kQueueFull};
    }

    auto state = std::make_unique<TargetReservation::State>(
        found->second, std::move(reserved.reservation));
    return {TargetReservation(std::move(state)), TargetReservationError::kNone};
}

Runtime::TargetAdmissionResult Runtime::admit_reserved(
    TargetReservation&& target_reservation, protocol::Message message,
    TargetDeliveryFailureHandler failure_handler,
    std::function<void()> completion_handler) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (lifecycle_state_ != LifecycleState::kRunning || !target_reservation) {
        return {DeliveryResult::kInvalidEnvelope, false};
    }

    if (target_reservation.state_->target->name !=
        message.envelope.target_node) {
        return {DeliveryResult::kInvalidEnvelope, false};
    }

    const auto result = ledger_.observe(message.envelope);
    if (result != DeliveryResult::kAccepted) {
        return {result, false};
    }

    auto state = std::move(target_reservation.state_);
    const auto committed = state->reservation.commit(
        TargetJob{std::move(message), std::move(failure_handler),
                  std::move(completion_handler)});
    if (committed != QueueCommitResult::kCommitted) {
        return {DeliveryResult::kInvalidEnvelope, false};
    }
    return {DeliveryResult::kAccepted, true};
}

void Runtime::worker_loop(
    const std::shared_ptr<TargetRegistration>& target) noexcept {
    while (const auto job = target->queue.wait_pop()) {
        if (deadline_expired(job->message.envelope, clock_)) {
            if (job->failure_handler) {
                job->failure_handler(TargetDeliveryFailure::kDeadlineExceeded);
            }
            if (job->completion_handler) {
                job->completion_handler();
            }
            continue;
        }

        // Exit/Cancel 只阻止尚未开始的工作；已经进入 handler 的副作用不能假装回滚。
        const auto cancellation = cancellations_
                                      .token_for(job->message.envelope.session_id,
                                                 job->message.envelope.work_id)
                                      .reason();
        if (cancellation != CancellationReason::kNone) {
            if (job->failure_handler) {
                job->failure_handler(
                    cancellation == CancellationReason::kSession
                        ? TargetDeliveryFailure::kSessionCancelled
                        : TargetDeliveryFailure::kWorkCancelled);
            }
            if (job->completion_handler) {
                job->completion_handler();
            }
            continue;
        }

        const auto handling_started_ns = clock_.now_monotonic_ns();
        const auto result = target->node->on_message(job->message);
        const auto handling_finished_ns = clock_.now_monotonic_ns();
        // 只统计实际进入业务 handler 的时间；排队等待和取消前置检查不混入处理耗时。
        target->handling_duration_ns_total.fetch_add(
            handling_finished_ns - handling_started_ns,
            std::memory_order_relaxed);
        target->handling_count.fetch_add(1, std::memory_order_relaxed);
        switch (result) {
            case MessageHandlingResult::kHandled:
                break;
            case MessageHandlingResult::kUnsupportedTopic:
                if (job->failure_handler) {
                    job->failure_handler(TargetDeliveryFailure::kUnsupportedTopic);
                }
                break;
            case MessageHandlingResult::kInvalidPayload:
                if (job->failure_handler) {
                    job->failure_handler(TargetDeliveryFailure::kInvalidPayload);
                }
                break;
        }
        if (job->completion_handler) {
            job->completion_handler();
        }
    }
}

void Runtime::cancel_session(std::string_view session_id) {
    ledger_.cancel_session(session_id);
    static_cast<void>(cancellations_.cancel_session(session_id));
    metrics_.increment("runtime.session_cancelled");
}

void Runtime::cancel_work(std::string_view session_id,
                          std::string_view work_id) {
    ledger_.cancel_work(session_id, work_id);
    static_cast<void>(cancellations_.cancel_work(session_id, work_id));
    metrics_.increment("runtime.work_cancelled");
}

bool Runtime::running() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return lifecycle_state_ == LifecycleState::kRunning;
}

std::uint64_t Runtime::metric_value(std::string_view name) const {
    return metrics_.value(name);
}

std::optional<Runtime::TargetStats> Runtime::target_stats(
    std::string_view target_node) const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const auto found = targets_by_name_.find(std::string(target_node));
    if (found == targets_by_name_.end()) {
        return std::nullopt;
    }

    const auto& target = *found->second;
    return TargetStats{
        target.queue.size(),
        target.queue_full_rejections.load(std::memory_order_relaxed),
        target.handling_count.load(std::memory_order_relaxed),
        target.handling_duration_ns_total.load(std::memory_order_relaxed)};
}

std::string_view to_string(RuntimeError error) noexcept {
    switch (error) {
        case RuntimeError::kNone:
            return "none";
        case RuntimeError::kInvalidNode:
            return "invalid_node";
        case RuntimeError::kTargetNodeRequiresTargetRegistration:
            return "target_node_requires_target_registration";
        case RuntimeError::kInvalidTargetQueueCapacity:
            return "invalid_target_queue_capacity";
        case RuntimeError::kEmptyTargetNodeName:
            return "empty_target_node_name";
        case RuntimeError::kDuplicateTargetNodeName:
            return "duplicate_target_node_name";
        case RuntimeError::kAlreadyStarted:
            return "already_started";
        case RuntimeError::kNodeStartFailure:
            return "node_start_failure";
        case RuntimeError::kTargetWorkerStartFailure:
            return "target_worker_start_failure";
        case RuntimeError::kLifecycleEnded:
            return "lifecycle_ended";
    }

    return "unknown";
}

}  // namespace cabinflow::runtime
