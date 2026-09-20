#include <cabinflow/transport/zmq/zmq_transport.hpp>

#include <zmq.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cabinflow/protocol/message_codec.hpp>

namespace cabinflow::transport::detail {
namespace {

constexpr auto kIoPollInterval = std::chrono::milliseconds(10);

[[noreturn]] void throw_zmq_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " +
                             zmq_strerror(zmq_errno()));
}

bool receive_frame(void* socket, std::string* frame) {
    zmq_msg_t message;
    if (zmq_msg_init(&message) != 0) {
        throw_zmq_error("zmq_msg_init");
    }

    const int received = zmq_msg_recv(&message, socket, ZMQ_DONTWAIT);
    if (received == -1) {
        const int error = zmq_errno();
        zmq_msg_close(&message);
        if (error == EAGAIN || error == EINTR) {
            return false;
        }
        throw_zmq_error("zmq_msg_recv");
    }

    frame->assign(static_cast<const char*>(zmq_msg_data(&message)),
                  zmq_msg_size(&message));
    if (zmq_msg_close(&message) != 0) {
        throw_zmq_error("zmq_msg_close");
    }
    return true;
}

bool has_more_frames(void* socket) {
    int more = 0;
    std::size_t more_size = sizeof(more);
    if (zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size) != 0) {
        throw_zmq_error("zmq_getsockopt(ZMQ_RCVMORE)");
    }
    return more != 0;
}

void discard_remaining_frames(void* socket) {
    while (has_more_frames(socket)) {
        std::string ignored;
        if (!receive_frame(socket, &ignored)) {
            return;
        }
    }
}

}  // namespace

class ZmqSubscription;

class ZmqTransportState final
    : public std::enable_shared_from_this<ZmqTransportState> {
public:
    explicit ZmqTransportState(ZmqTransportConfig config)
        : config_(std::move(config)) {
        if (config_.bind_endpoint.empty() && config_.connect_endpoint.empty()) {
            throw std::invalid_argument(
                "ZmqTransport requires a bind or connect endpoint");
        }
    }

    ~ZmqTransportState() { stop(); }

    ZmqTransportState(const ZmqTransportState&) = delete;
    ZmqTransportState& operator=(const ZmqTransportState&) = delete;

    [[nodiscard]] std::string start() {
        std::promise<std::string> started;
        auto started_result = started.get_future();

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callbacks_running_ = true;
        }
        callback_thread_ = std::thread(&ZmqTransportState::callback_loop, this);
        io_thread_ = std::thread(&ZmqTransportState::io_loop, this,
                                 std::move(started));

        try {
            return started_result.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    [[nodiscard]] TransportError publish(const protocol::Message& message) {
        if (message.envelope.topic.empty()) {
            return TransportError::kInvalidTopic;
        }

        auto encoded = protocol::encode_message(message);
        if (!encoded) {
            return TransportError::kCodecFailure;
        }

        Command command;
        command.type = CommandType::kPublish;
        command.topic = message.envelope.topic;
        command.encoded_message = std::move(encoded.bytes);
        return submit(std::move(command));
    }

    [[nodiscard]] SubscribeResult subscribe(std::string_view topic,
                                             MessageHandler handler);

    void unsubscribe(std::size_t subscription_id) noexcept {
        Command command;
        command.type = CommandType::kUnsubscribe;
        command.subscription_id = subscription_id;
        static_cast<void>(submit(std::move(command)));
    }

    [[nodiscard]] bool active() const noexcept {
        std::lock_guard<std::mutex> lock(command_mutex_);
        return accepting_commands_;
    }

    void stop() noexcept {
        std::deque<Command> pending_commands;
        bool notify_io = false;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            if (accepting_commands_ || !stop_requested_) {
                accepting_commands_ = false;
                stop_requested_ = true;
                pending_commands.swap(commands_);
                notify_io = true;
            }
        }

        for (auto& command : pending_commands) {
            command.completion.set_value(TransportError::kClosed);
        }
        if (notify_io) {
            command_cv_.notify_all();
        }

        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callbacks_running_ = false;
            callbacks_.clear();
        }
        callback_cv_.notify_all();

        if (callback_thread_.joinable()) {
            callback_thread_.join();
        }
    }

private:
    enum class CommandType {
        kPublish,
        kSubscribe,
        kUnsubscribe,
    };

    struct Command {
        CommandType type{CommandType::kPublish};
        std::size_t subscription_id{0};
        std::string topic;
        std::string encoded_message;
        MessageHandler handler;
        std::promise<TransportError> completion;
    };

    struct Subscriber {
        std::string topic;
        MessageHandler handler;
    };

    struct CallbackInvocation {
        MessageHandler handler;
        protocol::Message message;
    };

    // API threads only enqueue commands; the I/O thread is the sole owner of
    // both sockets, which is the ZeroMQ thread-safety invariant.
    [[nodiscard]] TransportError submit(Command command) noexcept {
        auto completion = command.completion.get_future();
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            if (!accepting_commands_) {
                return TransportError::kClosed;
            }
            commands_.push_back(std::move(command));
        }
        command_cv_.notify_one();
        return completion.get();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        std::lock_guard<std::mutex> lock(command_mutex_);
        return stop_requested_;
    }

    void setup_sockets() {
        context_ = zmq_ctx_new();
        if (context_ == nullptr) {
            throw_zmq_error("zmq_ctx_new");
        }
        if (zmq_ctx_set(context_, ZMQ_IO_THREADS, 1) != 0) {
            throw_zmq_error("zmq_ctx_set");
        }

        const int linger_ms = 0;
        if (!config_.bind_endpoint.empty()) {
            publisher_ = zmq_socket(context_, ZMQ_PUB);
            if (publisher_ == nullptr) {
                throw_zmq_error("zmq_socket(ZMQ_PUB)");
            }
            if (zmq_setsockopt(publisher_, ZMQ_LINGER, &linger_ms,
                               sizeof(linger_ms)) != 0) {
                throw_zmq_error("zmq_setsockopt(ZMQ_LINGER)");
            }
            if (zmq_bind(publisher_, config_.bind_endpoint.c_str()) != 0) {
                throw_zmq_error("zmq_bind");
            }
            bound_endpoint_ = read_bound_endpoint();
        }

        if (!config_.connect_endpoint.empty()) {
            subscriber_ = zmq_socket(context_, ZMQ_SUB);
            if (subscriber_ == nullptr) {
                throw_zmq_error("zmq_socket(ZMQ_SUB)");
            }
            if (zmq_setsockopt(subscriber_, ZMQ_LINGER, &linger_ms,
                               sizeof(linger_ms)) != 0) {
                throw_zmq_error("zmq_setsockopt(ZMQ_LINGER)");
            }
            if (zmq_connect(subscriber_, config_.connect_endpoint.c_str()) !=
                0) {
                throw_zmq_error("zmq_connect");
            }
        }
    }

    [[nodiscard]] std::string read_bound_endpoint() const {
        std::vector<char> endpoint(256, '\0');
        std::size_t endpoint_size = endpoint.size();
        if (zmq_getsockopt(publisher_, ZMQ_LAST_ENDPOINT, endpoint.data(),
                           &endpoint_size) != 0) {
            throw_zmq_error("zmq_getsockopt(ZMQ_LAST_ENDPOINT)");
        }

        if (endpoint_size > 0 && endpoint[endpoint_size - 1] == '\0') {
            --endpoint_size;
        }
        return std::string(endpoint.data(), endpoint_size);
    }

    void close_sockets() noexcept {
        if (subscriber_ != nullptr) {
            zmq_close(subscriber_);
            subscriber_ = nullptr;
        }
        if (publisher_ != nullptr) {
            zmq_close(publisher_);
            publisher_ = nullptr;
        }
        if (context_ != nullptr) {
            zmq_ctx_term(context_);
            context_ = nullptr;
        }
    }

    void io_loop(std::promise<std::string> started) noexcept {
        bool started_successfully = false;
        try {
            setup_sockets();
            started.set_value(bound_endpoint_);
            started_successfully = true;

            while (!stop_requested()) {
                process_pending_commands();
                if (stop_requested()) {
                    break;
                }

                if (subscriber_ == nullptr) {
                    std::unique_lock<std::mutex> lock(command_mutex_);
                    command_cv_.wait_for(lock, kIoPollInterval, [this] {
                        return stop_requested_ || !commands_.empty();
                    });
                    continue;
                }

                zmq_pollitem_t item{subscriber_, 0, ZMQ_POLLIN, 0};
                const int polled = zmq_poll(&item, 1, kIoPollInterval.count());
                if (polled == -1) {
                    if (zmq_errno() == EINTR) {
                        continue;
                    }
                    throw_zmq_error("zmq_poll");
                }
                if ((item.revents & ZMQ_POLLIN) != 0) {
                    receive_available();
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(command_mutex_);
                accepting_commands_ = false;
                stop_requested_ = true;
            }
            if (!started_successfully) {
                started.set_exception(std::current_exception());
            }
        }

        complete_pending_commands();
        close_sockets();
    }

    void process_pending_commands() {
        std::deque<Command> commands;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            commands.swap(commands_);
        }

        for (auto& command : commands) {
            if (stop_requested()) {
                command.completion.set_value(TransportError::kClosed);
                continue;
            }

            TransportError result = TransportError::kIoFailure;
            switch (command.type) {
                case CommandType::kPublish:
                    result = publish_from_io(command);
                    break;
                case CommandType::kSubscribe:
                    result = subscribe_from_io(command);
                    break;
                case CommandType::kUnsubscribe:
                    result = unsubscribe_from_io(command.subscription_id);
                    break;
            }
            command.completion.set_value(result);
        }
    }

    [[nodiscard]] TransportError publish_from_io(const Command& command) {
        if (publisher_ == nullptr) {
            return TransportError::kUnavailable;
        }
        if (zmq_send(publisher_, command.topic.data(), command.topic.size(),
                     ZMQ_SNDMORE | ZMQ_DONTWAIT) == -1) {
            return TransportError::kIoFailure;
        }
        if (zmq_send(publisher_, command.encoded_message.data(),
                     command.encoded_message.size(), ZMQ_DONTWAIT) == -1) {
            return TransportError::kIoFailure;
        }
        return TransportError::kNone;
    }

    [[nodiscard]] TransportError subscribe_from_io(Command& command) {
        if (subscriber_ == nullptr) {
            return TransportError::kUnavailable;
        }

        const auto topic_count = subscribed_topics_.find(command.topic);
        if (topic_count == subscribed_topics_.end()) {
            if (zmq_setsockopt(subscriber_, ZMQ_SUBSCRIBE, command.topic.data(),
                               command.topic.size()) != 0) {
                return TransportError::kIoFailure;
            }
            subscribed_topics_.emplace(command.topic, 1);
        } else {
            ++topic_count->second;
        }

        subscribers_.emplace(command.subscription_id,
                             Subscriber{std::move(command.topic),
                                        std::move(command.handler)});
        return TransportError::kNone;
    }

    [[nodiscard]] TransportError unsubscribe_from_io(
        std::size_t subscription_id) {
        const auto subscriber = subscribers_.find(subscription_id);
        if (subscriber == subscribers_.end()) {
            return TransportError::kNone;
        }

        const std::string topic = subscriber->second.topic;
        subscribers_.erase(subscriber);

        const auto topic_count = subscribed_topics_.find(topic);
        if (topic_count == subscribed_topics_.end()) {
            return TransportError::kIoFailure;
        }
        if (--topic_count->second != 0) {
            return TransportError::kNone;
        }

        subscribed_topics_.erase(topic_count);
        if (zmq_setsockopt(subscriber_, ZMQ_UNSUBSCRIBE, topic.data(),
                           topic.size()) != 0) {
            return TransportError::kIoFailure;
        }
        return TransportError::kNone;
    }

    void receive_available() {
        while (true) {
            std::string topic;
            if (!receive_frame(subscriber_, &topic)) {
                return;
            }
            if (!has_more_frames(subscriber_)) {
                continue;
            }

            std::string encoded_message;
            if (!receive_frame(subscriber_, &encoded_message)) {
                return;
            }
            if (has_more_frames(subscriber_)) {
                discard_remaining_frames(subscriber_);
                continue;
            }

            auto decoded = protocol::decode_message(encoded_message);
            if (!decoded || decoded.message.envelope.topic != topic) {
                continue;
            }
            enqueue_callbacks(std::move(decoded.message));
        }
    }

    void enqueue_callbacks(protocol::Message message) {
        std::vector<MessageHandler> handlers;
        for (const auto& [subscription_id, subscriber] : subscribers_) {
            static_cast<void>(subscription_id);
            if (subscriber.topic == message.envelope.topic) {
                handlers.push_back(subscriber.handler);
            }
        }

        if (handlers.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (!callbacks_running_) {
                return;
            }
            for (auto& handler : handlers) {
                callbacks_.push_back(
                    CallbackInvocation{std::move(handler), message});
            }
        }
        callback_cv_.notify_one();
    }

    void callback_loop() {
        while (true) {
            CallbackInvocation callback;
            {
                std::unique_lock<std::mutex> lock(callback_mutex_);
                callback_cv_.wait(lock, [this] {
                    return !callbacks_running_ || !callbacks_.empty();
                });
                if (!callbacks_running_) {
                    return;
                }
                callback = std::move(callbacks_.front());
                callbacks_.pop_front();
            }

            // 不吞掉业务回调异常；异常处理者不能被伪装成已成功消费消息。
            callback.handler(callback.message);
        }
    }

    void complete_pending_commands() noexcept {
        std::deque<Command> pending_commands;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            accepting_commands_ = false;
            stop_requested_ = true;
            pending_commands.swap(commands_);
        }
        for (auto& command : pending_commands) {
            command.completion.set_value(TransportError::kClosed);
        }
        command_cv_.notify_all();
    }

    ZmqTransportConfig config_;

    mutable std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::deque<Command> commands_;
    bool accepting_commands_{true};
    bool stop_requested_{false};
    std::atomic<std::size_t> next_subscription_id_{1};

    std::thread io_thread_;
    std::thread callback_thread_;
    void* context_{nullptr};
    void* publisher_{nullptr};
    void* subscriber_{nullptr};
    std::string bound_endpoint_;
    std::unordered_map<std::size_t, Subscriber> subscribers_;
    std::unordered_map<std::string, std::size_t> subscribed_topics_;

    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;
    std::deque<CallbackInvocation> callbacks_;
    bool callbacks_running_{false};
};

class ZmqSubscription final : public Subscription {
public:
    ZmqSubscription(std::weak_ptr<ZmqTransportState> state,
                    std::size_t subscription_id) noexcept
        : state_(std::move(state)), subscription_id_(subscription_id) {}

    ~ZmqSubscription() override { unsubscribe(); }

    void unsubscribe() noexcept override {
        if (!active_.exchange(false)) {
            return;
        }
        if (const auto state = state_.lock()) {
            state->unsubscribe(subscription_id_);
        }
    }

    [[nodiscard]] bool active() const noexcept override {
        if (!active_.load()) {
            return false;
        }
        const auto state = state_.lock();
        return state != nullptr && state->active();
    }

private:
    std::weak_ptr<ZmqTransportState> state_;
    const std::size_t subscription_id_;
    std::atomic<bool> active_{true};
};

SubscribeResult ZmqTransportState::subscribe(std::string_view topic,
                                              MessageHandler handler) {
    if (topic.empty()) {
        return {nullptr, TransportError::kInvalidTopic};
    }
    if (!handler) {
        return {nullptr, TransportError::kEmptyHandler};
    }

    const std::size_t subscription_id = next_subscription_id_.fetch_add(1);
    Command command;
    command.type = CommandType::kSubscribe;
    command.subscription_id = subscription_id;
    command.topic = std::string(topic);
    command.handler = std::move(handler);

    const TransportError result = submit(std::move(command));
    if (result != TransportError::kNone) {
        return {nullptr, result};
    }

    return {std::make_unique<ZmqSubscription>(shared_from_this(),
                                               subscription_id),
            TransportError::kNone};
}

}  // namespace cabinflow::transport::detail

namespace cabinflow::transport {

ZmqTransport::ZmqTransport(ZmqTransportConfig config)
    : state_(std::make_shared<detail::ZmqTransportState>(std::move(config))) {
    bound_endpoint_ = state_->start();
}

ZmqTransport::~ZmqTransport() = default;

TransportError ZmqTransport::publish(const protocol::Message& message) {
    return state_->publish(message);
}

SubscribeResult ZmqTransport::subscribe(std::string_view topic,
                                          MessageHandler handler) {
    return state_->subscribe(topic, std::move(handler));
}

std::string_view ZmqTransport::bound_endpoint() const noexcept {
    return bound_endpoint_;
}

}  // namespace cabinflow::transport
