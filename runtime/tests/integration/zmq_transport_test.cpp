#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <cabinflow/protocol/message.hpp>
#include <cabinflow/transport/transport_error.hpp>
#include <cabinflow/transport/zmq/zmq_transport.hpp>

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

cabinflow::protocol::Message make_message() {
    cabinflow::protocol::Message message;
    message.envelope.message_id = "zmq-message-1";
    message.envelope.trace_id = "zmq-trace-1";
    message.envelope.session_id = "driver-session";
    message.envelope.work_id = "media-work";
    message.envelope.source_node = "text-source";
    message.envelope.target_node = "echo-processor";
    message.envelope.topic = "voice.partial";
    message.envelope.sequence = 9;
    message.payload = "play jazz";
    return message;
}

}  // namespace

int main() {
    using cabinflow::transport::TransportError;
    using cabinflow::transport::ZmqTransport;
    using cabinflow::transport::ZmqTransportConfig;

    ZmqTransportConfig producer_config;
    producer_config.bind_endpoint = "tcp://127.0.0.1:*";
    ZmqTransport producer(std::move(producer_config));
    if (!expect(!producer.bound_endpoint().empty(), "bound endpoint")) {
        return 1;
    }

    ZmqTransportConfig consumer_config;
    consumer_config.connect_endpoint = std::string(producer.bound_endpoint());
    ZmqTransport consumer(std::move(consumer_config));

    std::mutex delivery_mutex;
    std::condition_variable delivery_cv;
    bool delivered = false;
    int delivery_count = 0;
    cabinflow::protocol::Message received;
    std::thread::id callback_thread;

    auto subscription = consumer.subscribe("voice.partial", [&](const auto& message) {
        std::lock_guard<std::mutex> lock(delivery_mutex);
        received = message;
        callback_thread = std::this_thread::get_id();
        ++delivery_count;
        delivered = true;
        delivery_cv.notify_one();
    });
    if (!expect(static_cast<bool>(subscription), "subscription") ||
        !expect(subscription.subscription->active(), "active subscription")) {
        return 1;
    }

    // PUB/SUB 没有连接完成确认；这里显式留出握手窗口，而非在 Transport 内隐藏重试。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::thread::id publisher_thread = std::this_thread::get_id();
    if (!expect(producer.publish(make_message()) == TransportError::kNone,
                "publish succeeds")) {
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(delivery_mutex);
        if (!delivery_cv.wait_for(lock, std::chrono::seconds(1), [&] {
                return delivered;
            })) {
            std::cerr << "expectation failed: message delivered\n";
            return 1;
        }
    }

    if (!expect(received.envelope.message_id == "zmq-message-1",
                "message id preserved") ||
        !expect(received.envelope.trace_id == "zmq-trace-1",
                "trace id preserved") ||
        !expect(received.envelope.sequence == 9, "sequence preserved") ||
        !expect(received.payload == "play jazz", "payload preserved") ||
        !expect(callback_thread != publisher_thread, "callback thread ownership")) {
        return 1;
    }

    subscription.subscription->unsubscribe();
    if (!expect(!subscription.subscription->active(), "unsubscribe")) {
        return 1;
    }

    if (!expect(producer.publish(make_message()) == TransportError::kNone,
                "publish after unsubscribe")) {
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(delivery_mutex);
        if (delivery_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return delivery_count > 1;
            })) {
            std::cerr << "expectation failed: unsubscribed callback skipped\n";
            return 1;
        }
    }

    return 0;
}
