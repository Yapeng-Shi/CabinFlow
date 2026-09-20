#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cabinflow::transport::detail {

struct Subscriber {
    std::string topic;
    MessageHandler handler;
};

struct InMemoryTransportState {
    std::mutex mutex;
    std::size_t next_subscription_id{1};
    std::unordered_map<std::size_t, Subscriber> subscribers;
};

}  // namespace cabinflow::transport::detail

namespace cabinflow::transport {
namespace {

class InMemorySubscription final : public Subscription {
public:
    InMemorySubscription(std::weak_ptr<detail::InMemoryTransportState> state,
                         std::size_t subscription_id) noexcept
        : state_(std::move(state)), subscription_id_(subscription_id) {}

    ~InMemorySubscription() override { unsubscribe(); }

    void unsubscribe() noexcept override {
        if (!active_.exchange(false)) {
            return;
        }

        if (const auto state = state_.lock()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->subscribers.erase(subscription_id_);
        }
    }

    [[nodiscard]] bool active() const noexcept override {
        return active_.load() && !state_.expired();
    }

private:
    std::weak_ptr<detail::InMemoryTransportState> state_;
    std::size_t subscription_id_;
    std::atomic<bool> active_{true};
};

}  // namespace

InMemoryTransport::InMemoryTransport()
    : state_(std::make_shared<detail::InMemoryTransportState>()) {}

InMemoryTransport::~InMemoryTransport() = default;

TransportError InMemoryTransport::publish(
    const protocol::Message& message) {
    if (message.envelope.topic.empty()) {
        return TransportError::kInvalidTopic;
    }

    std::vector<MessageHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [subscription_id, subscriber] : state_->subscribers) {
            static_cast<void>(subscription_id);
            if (subscriber.topic == message.envelope.topic) {
                handlers.push_back(subscriber.handler);
            }
        }
    }

    for (const auto& handler : handlers) {
        handler(message);
    }

    return TransportError::kNone;
}

SubscribeResult InMemoryTransport::subscribe(std::string_view topic,
                                              MessageHandler handler) {
    if (topic.empty()) {
        return {nullptr, TransportError::kInvalidTopic};
    }

    if (!handler) {
        return {nullptr, TransportError::kEmptyHandler};
    }

    detail::Subscriber subscriber{std::string(topic), std::move(handler)};
    std::size_t subscription_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        subscription_id = state_->next_subscription_id++;
    }

    auto subscription =
        std::make_unique<InMemorySubscription>(state_, subscription_id);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->subscribers.emplace(subscription_id, std::move(subscriber));
    }

    return {std::move(subscription),
            TransportError::kNone};
}

}  // namespace cabinflow::transport
