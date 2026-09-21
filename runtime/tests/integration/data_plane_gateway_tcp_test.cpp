#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <cockpit_text.pb.h>
#include <control.pb.h>
#include <delivery.pb.h>

#include <cabinflow/agent/dialogue_text_node.hpp>
#include <cabinflow/gateway/control_gateway.hpp>
#include <cabinflow/gateway/runtime_message_framer.hpp>
#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "recording_logger.hpp"

namespace {

using namespace std::chrono_literals;
using cabinflow::gateway::RuntimeMessageFramer;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                static_cast<void>(::close(value_));
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_{-1};
};

class LoopThread final {
public:
    LoopThread() : thread_([this] {
        cabinflow::net::EventLoop loop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = &loop;
            ready_ = true;
        }
        changed_.notify_all();
        loop.loop();
    }) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return ready_; });
    }

    ~LoopThread() {
        if (thread_.joinable()) {
            loop().quit();
            thread_.join();
        }
    }

    template <typename Task>
    void run_and_wait(Task&& task) {
        auto completed = std::make_shared<std::promise<void>>();
        auto result = completed->get_future();
        loop().queue_in_loop([completed, task = std::forward<Task>(task)]() mutable {
            try {
                task();
                completed->set_value();
            } catch (...) {
                completed->set_exception(std::current_exception());
            }
        });
        result.get();
    }

    [[nodiscard]] cabinflow::net::EventLoop& loop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loop_ == nullptr) {
            throw std::logic_error("event loop is unavailable");
        }
        return *loop_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    cabinflow::net::EventLoop* loop_{nullptr};
    bool ready_{false};
    std::thread thread_;
};

class GatewayFixture final {
public:
    GatewayFixture()
        : runtime_(transport_, clock_, logger_) {
        require(runtime_.add_target_node(
                    std::make_unique<cabinflow::agent::DialogueTextNode>(), 8) ==
                    cabinflow::runtime::RuntimeError::kNone,
                "dialogue target registers");
        require(runtime_.start() == cabinflow::runtime::RuntimeError::kNone,
                "runtime starts before gateway accepts data");
        loop_thread_.run_and_wait([this] {
            gateway_ = std::make_unique<cabinflow::gateway::ControlGateway>(
                loop_thread_.loop(), "data-gateway-test", "127.0.0.1", 0,
                registry_, runtime_, clock_, 500);
            gateway_->start();
            port_ = gateway_->bound_port();
        });
    }

    ~GatewayFixture() {
        // worker 可能还在生成 DeliveryError，因此先停止 Runtime，再释放 Gateway。
        runtime_.stop();
        loop_thread_.run_and_wait([this] {
            gateway_->stop();
            gateway_.reset();
        });
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] cabinflow::runtime::SteadyClock& clock() noexcept {
        return clock_;
    }
    [[nodiscard]] cabinflow::test::RecordingLogger& logger() noexcept {
        return logger_;
    }

private:
    LoopThread loop_thread_;
    cabinflow::runtime::SteadyClock clock_;
    cabinflow::transport::InMemoryTransport transport_;
    cabinflow::test::RecordingLogger logger_;
    cabinflow::runtime::Runtime runtime_;
    cabinflow::runtime::UnitRegistry registry_;
    std::unique_ptr<cabinflow::gateway::ControlGateway> gateway_;
    std::uint16_t port_{0};
};

FileDescriptor connect_client(std::uint16_t port) {
    FileDescriptor client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (client.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "create client socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect data gateway");
    }
    return client;
}

void send_all(int descriptor, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent != bytes.size()) {
        const auto result = ::send(descriptor, bytes.data() + sent, bytes.size() - sent,
                                   MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "send gateway frame");
    }
}

void send_message(int descriptor, const Message& message, bool split_header = false) {
    const auto frame = RuntimeMessageFramer::encode(message);
    require(static_cast<bool>(frame), "encode runtime message");
    if (split_header) {
        send_all(descriptor, std::string_view(frame.bytes).substr(0, 2));
        send_all(descriptor, std::string_view(frame.bytes).substr(2));
        return;
    }
    send_all(descriptor, frame.bytes);
}

std::string read_exact(int descriptor, std::size_t expected_size) {
    std::string bytes(expected_size, '\0');
    std::size_t received = 0;
    while (received != expected_size) {
        pollfd ready{descriptor, POLLIN, 0};
        require(::poll(&ready, 1, 1'000) == 1 && (ready.revents & POLLIN) != 0,
                "timed out waiting for gateway response");
        const auto result = ::recv(descriptor, bytes.data() + received,
                                   bytes.size() - received, 0);
        require(result > 0, "gateway closed before response completed");
        received += static_cast<std::size_t>(result);
    }
    return bytes;
}

Message read_message(int descriptor) {
    const auto header = read_exact(descriptor, cabinflow::gateway::kFrameHeaderBytes);
    const auto body_size =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    RuntimeMessageFramer framer;
    const auto decoded = framer.feed(header + read_exact(descriptor, body_size));
    require(decoded && decoded.messages.size() == 1, "response frame decodes once");
    return decoded.messages.front();
}

void expect_no_response(int descriptor) {
    pollfd ready{descriptor, POLLIN | POLLHUP, 0};
    require(::poll(&ready, 1, 100) == 0,
            "handled data must not produce a success acknowledgement");
}

void wait_for_dialogue_event(cabinflow::test::RecordingLogger& logger,
                             std::size_t expected_count) {
    for (int attempt = 0; attempt != 100; ++attempt) {
        std::size_t count = 0;
        for (const auto& event : logger.snapshot()) {
            if (event.component == "dialogue" && event.name == "text_input_received") {
                ++count;
            }
        }
        if (count >= expected_count) {
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("dialogue target did not receive typed text");
}

Message make_control(std::string message_id, std::string session_id,
                     std::string work_id,
                     const cabinflow::protocol::v1::ControlRequest& request) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-control";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "tcp-client";
    message.envelope.target_node = "control-gateway";
    message.envelope.topic = "control.request";
    message.envelope.ttl_ms = 1'000;
    require(request.SerializeToString(&message.payload), "serialize control request");
    return message;
}

std::string register_and_setup(int descriptor) {
    cabinflow::protocol::v1::ControlRequest registration;
    registration.mutable_register_unit()->set_unit_id("dialogue.primary");
    registration.mutable_register_unit()->add_capabilities("dialogue");
    registration.mutable_register_unit()->set_max_concurrent_work(1);
    send_message(descriptor, make_control("register-dialogue", "", "", registration));
    const auto registered = read_message(descriptor);
    cabinflow::protocol::v1::ControlResponse registered_response;
    require(registered_response.ParseFromString(registered.payload) &&
                registered_response.has_register_unit(),
            "dialogue unit registration succeeds through control service");

    cabinflow::protocol::v1::ControlRequest setup;
    setup.mutable_setup()->set_unit_id("dialogue.primary");
    send_message(descriptor, make_control("setup-dialogue", "session-text", "", setup));
    const auto setup_message = read_message(descriptor);
    cabinflow::protocol::v1::ControlResponse setup_response;
    require(setup_response.ParseFromString(setup_message.payload) &&
                setup_response.has_setup() &&
                !setup_response.setup().work().work_id().empty() &&
                setup_message.envelope.work_id ==
                    setup_response.setup().work().work_id(),
            "setup creates work before data reaches SessionLedger");
    return setup_response.setup().work().work_id();
}

Message make_data(std::string message_id, std::string work_id,
                  std::string target, std::string topic,
                  std::uint64_t sequence, std::uint64_t created_ns,
                  std::uint32_t ttl_ms, std::string payload,
                  bool is_final = false) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-data";
    message.envelope.session_id = "session-text";
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "tcp-client";
    message.envelope.target_node = std::move(target);
    message.envelope.topic = std::move(topic);
    message.envelope.kind = MessageKind::kData;
    message.envelope.sequence = sequence;
    message.envelope.created_monotonic_ns = created_ns;
    message.envelope.ttl_ms = ttl_ms;
    message.envelope.is_final = is_final;
    message.payload = std::move(payload);
    return message;
}

std::string text_payload(std::string text) {
    cabinflow::agent::v1::TextInput input;
    input.set_text(std::move(text));
    std::string payload;
    require(input.SerializeToString(&payload), "serialize TextInput");
    return payload;
}

cabinflow::protocol::v1::DeliveryError read_delivery_error(int descriptor,
                                                            std::string_view request_id,
                                                            cabinflow::protocol::v1::DeliveryErrorCode code) {
    const auto response = read_message(descriptor);
    require(response.envelope.topic == "runtime.delivery.error" &&
                response.envelope.kind == MessageKind::kError && response.envelope.is_final,
            "data rejection must use the typed delivery error envelope");
    require(response.payload.empty() || response.payload.front() != '{',
            "gateway must not emit a JSON compatibility response");
    cabinflow::protocol::v1::DeliveryError error;
    require(error.ParseFromString(response.payload) &&
                error.request_message_id() == request_id && error.code() == code,
            "delivery error payload code or correlation changed");
    return error;
}

void test_typed_data_plane_over_tcp() {
    GatewayFixture fixture;
    const auto client = connect_client(fixture.port());
    const auto work_id = register_and_setup(client.get());

    const auto now = fixture.clock().now_monotonic_ns();
    send_message(client.get(), make_data("text-0", work_id, "dialogue.primary",
                                         "cockpit.text.input", 0, now, 1'000,
                                         text_payload("turn on seat heating")), true);
    wait_for_dialogue_event(fixture.logger(), 1);
    expect_no_response(client.get());

    send_message(client.get(), make_data("text-bad-proto", work_id, "dialogue.primary",
                                         "cockpit.text.input", 1, now, 1'000,
                                         "not a TextInput protobuf"));
    read_delivery_error(client.get(), "text-bad-proto",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client.get(), make_data("text-empty", work_id, "dialogue.primary",
                                         "cockpit.text.input", 2, now, 1'000,
                                         text_payload("")));
    read_delivery_error(client.get(), "text-empty",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client.get(), make_data("text-invalid-utf8", work_id,
                                         "dialogue.primary", "cockpit.text.input", 3,
                                         now, 1'000, text_payload("\xc3\x28")));
    read_delivery_error(client.get(), "text-invalid-utf8",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client.get(), make_data("unsupported-topic", work_id,
                                         "dialogue.primary", "cockpit.text.unsupported", 0,
                                         now, 1'000, text_payload("unsupported")));
    read_delivery_error(client.get(), "unsupported-topic",
                        cabinflow::protocol::v1::DELIVERY_ERROR_UNSUPPORTED_TOPIC);

    send_message(client.get(), make_data("unknown-target", work_id, "missing.target",
                                         "cockpit.text.unknown", 0, now, 1'000,
                                         text_payload("ignored")));
    read_delivery_error(client.get(), "unknown-target",
                        cabinflow::protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET);

    const auto expired_created = fixture.clock().now_monotonic_ns() - 2'000'000U;
    send_message(client.get(), make_data("expired-before-enqueue", work_id,
                                         "dialogue.primary", "cockpit.text.expired", 0,
                                         expired_created, 1, text_payload("expired")));
    read_delivery_error(client.get(), "expired-before-enqueue",
                        cabinflow::protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED);

    send_message(client.get(), make_data("text-final", work_id, "dialogue.primary",
                                         "cockpit.text.input", 4,
                                         fixture.clock().now_monotonic_ns(), 1'000,
                                         text_payload("final text"), true));
    wait_for_dialogue_event(fixture.logger(), 2);
    expect_no_response(client.get());

    send_message(client.get(), make_data("after-final", work_id, "dialogue.primary",
                                         "cockpit.text.input", 5,
                                         fixture.clock().now_monotonic_ns(), 1'000,
                                         text_payload("must reject")));
    read_delivery_error(client.get(), "after-final",
                        cabinflow::protocol::v1::DELIVERY_ERROR_STREAM_FINALIZED);
}

}  // namespace

int main() {
    try {
        test_typed_data_plane_over_tcp();
        std::cout << "data plane gateway tcp test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "data plane gateway tcp test failed: " << error.what() << '\n';
        return 1;
    }
}
