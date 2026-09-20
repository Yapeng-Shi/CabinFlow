#include <cabinflow/runtime/runtime.hpp>

#include <string_view>
#include <utility>

namespace cabinflow::runtime {

Runtime::Runtime(transport::ITransport& transport, Clock& clock,
                 observability::Logger& logger)
    : ledger_(clock),
      context_(transport, clock, ledger_, cancellations_, logger, metrics_) {}

Runtime::~Runtime() { stop(); }

RuntimeError Runtime::add_node(std::unique_ptr<Node> node) {
    if (node == nullptr) {
        return RuntimeError::kInvalidNode;
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
        return RuntimeError::kAlreadyStarted;
    }
    nodes_.push_back(std::move(node));
    return RuntimeError::kNone;
}

RuntimeError Runtime::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) {
        return RuntimeError::kAlreadyStarted;
    }

    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index]->start(context_) != RuntimeError::kNone) {
            metrics_.increment("runtime.node_start_failure");
            // 启动依赖按注册顺序建立，失败时按逆序释放，避免下游仍引用上游资源。
            while (started_node_count_ > 0) {
                nodes_[--started_node_count_]->stop();
            }
            return RuntimeError::kNodeStartFailure;
        }
        ++started_node_count_;
        metrics_.increment("runtime.node_started");
    }

    running_ = true;
    return RuntimeError::kNone;
}

void Runtime::stop() noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    while (started_node_count_ > 0) {
        nodes_[--started_node_count_]->stop();
        metrics_.increment("runtime.node_stopped");
    }
    running_ = false;
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
    return running_;
}

std::uint64_t Runtime::metric_value(std::string_view name) const {
    return metrics_.value(name);
}

std::string_view to_string(RuntimeError error) noexcept {
    switch (error) {
        case RuntimeError::kNone:
            return "none";
        case RuntimeError::kInvalidNode:
            return "invalid_node";
        case RuntimeError::kAlreadyStarted:
            return "already_started";
        case RuntimeError::kNodeStartFailure:
            return "node_start_failure";
    }

    return "unknown";
}

}  // namespace cabinflow::runtime
