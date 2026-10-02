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

#include <control.pb.h>

#include <cabinflow/gateway/control_gateway.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/gateway/runtime_message_framer.hpp>
#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

namespace {

using namespace std::chrono_literals;

using cabinflow::gateway::RuntimeMessageFramer;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
using cabinflow::protocol::v1::ControlRequest;
using cabinflow::protocol::v1::ControlResponse;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
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

class NullLogger final : public cabinflow::observability::Logger {
public:
    void log(const cabinflow::observability::Event&) override {}
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = nullptr;
        }
        changed_.notify_all();
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
            throw std::logic_error("event loop is not running");
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

struct GatewayHandle {
    std::unique_ptr<cabinflow::runtime::UnitRegistry> registry;
    std::unique_ptr<cabinflow::runtime::SteadyClock> clock;
    std::unique_ptr<cabinflow::transport::InMemoryTransport> transport;
    std::unique_ptr<NullLogger> logger;
    std::unique_ptr<cabinflow::runtime::Runtime> runtime;
    std::unique_ptr<cabinflow::gateway::ControlGateway> gateway;
    std::uint16_t port{0};
};

GatewayHandle start_gateway(LoopThread& loop_thread) {
    GatewayHandle handle;
    handle.registry = std::make_unique<cabinflow::runtime::UnitRegistry>();
    handle.clock = std::make_unique<cabinflow::runtime::SteadyClock>();
    handle.transport = std::make_unique<cabinflow::transport::InMemoryTransport>();
    handle.logger = std::make_unique<NullLogger>();
    handle.runtime = std::make_unique<cabinflow::runtime::Runtime>(
        *handle.transport, *handle.clock, *handle.logger);
    loop_thread.run_and_wait([&] {
        handle.gateway = std::make_unique<cabinflow::gateway::ControlGateway>(
            loop_thread.loop(), "control-gateway-test", "127.0.0.1", 0,
            *handle.registry, *handle.runtime, *handle.clock, 500);
        handle.gateway->set_worker_count(2);
        handle.gateway->start();
        handle.port = handle.gateway->bound_port();
    });
    return handle;
}

void stop_gateway(LoopThread& loop_thread, GatewayHandle* handle) {
    if (!handle->gateway) {
        return;
    }
    loop_thread.run_and_wait([handle] {
        handle->gateway->stop();
        handle->gateway.reset();
    });
}

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
        throw std::system_error(errno, std::generic_category(), "connect control gateway");
    }
    return client;
}

void send_all(int file_descriptor, std::string_view bytes) {
    std::size_t sent = 0;
    while (sent != bytes.size()) {
        const ssize_t result = ::send(file_descriptor, bytes.data() + sent,
                                      bytes.size() - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "send control frame");
    }
}

std::string read_exact(int file_descriptor, std::size_t bytes_to_read) {
    std::string bytes(bytes_to_read, '\0');
    std::size_t received = 0;
    while (received != bytes.size()) {
        pollfd ready{file_descriptor, POLLIN, 0};
        require(::poll(&ready, 1, 1'000) == 1 && (ready.revents & POLLIN) != 0,
                "timed out waiting for control response");
        const auto count = ::recv(file_descriptor, bytes.data() + received,
                                  bytes.size() - received, 0);
        require(count > 0, "connection closed before complete response");
        received += static_cast<std::size_t>(count);
    }
    return bytes;
}

Message read_message(int file_descriptor) {
    const auto header = read_exact(file_descriptor, cabinflow::gateway::kFrameHeaderBytes);
    const auto body_size =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    const auto body = read_exact(file_descriptor, body_size);

    RuntimeMessageFramer framer;
    const auto decoded = framer.feed(header + body);
    require(decoded && decoded.messages.size() == 1, "response frame did not decode");
    return decoded.messages.front();
}

void expect_peer_closed(int file_descriptor) {
    pollfd ready{file_descriptor, POLLIN | POLLHUP, 0};
    require(::poll(&ready, 1, 1'000) == 1 &&
                (ready.revents & (POLLIN | POLLHUP)) != 0,
            "gateway did not close the connection");
    char byte = '\0';
    require(::recv(file_descriptor, &byte, sizeof(byte), 0) == 0,
            "gateway kept connection open");
}

Message make_request(std::string message_id, std::string session_id,
                     std::string work_id, const ControlRequest& request,
                     std::uint16_t schema_version =
                         cabinflow::protocol::kCurrentSchemaVersion) {
    Message message;
    message.envelope.schema_version = schema_version;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-tcp";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "tcp-client";
    message.envelope.target_node = "control-gateway";
    message.envelope.topic = "control.request";
    message.envelope.kind = MessageKind::kData;
    message.envelope.created_monotonic_ns =
        cabinflow::runtime::SteadyClock{}.now_monotonic_ns();
    message.envelope.ttl_ms = 1000;
    require(request.SerializeToString(&message.payload), "serialize request payload");
    return message;
}

void send_request(int file_descriptor, const Message& request, bool split_header) {
    const auto frame = RuntimeMessageFramer::encode(request);
    require(static_cast<bool>(frame), "encode request frame");
    if (split_header) {
        send_all(file_descriptor, std::string_view(frame.bytes).substr(0, 2));
        send_all(file_descriptor, std::string_view(frame.bytes).substr(2));
        return;
    }
    send_all(file_descriptor, frame.bytes);
}

ControlResponse parse_control_response(const Message& message) {
    require(message.envelope.topic == "control.response", "response topic changed");
    ControlResponse response;
    require(response.ParseFromString(message.payload), "decode response payload");
    return response;
}

void test_control_gateway_over_tcp() {
    LoopThread loop_thread;
    GatewayHandle gateway = start_gateway(loop_thread);
    try {
        FileDescriptor client = connect_client(gateway.port);

        ControlRequest registration;
        registration.mutable_register_unit()->set_unit_id("asr.primary");
        registration.mutable_register_unit()->add_capabilities("speech");
        registration.mutable_register_unit()->set_max_concurrent_work(1);
        send_request(client.get(), make_request("register-1", "", "", registration), true);
        const auto registered_message = read_message(client.get());
        const auto registered = parse_control_response(registered_message);
        require(registered.has_register_unit() &&
                    registered.request_message_id() == "register-1" &&
                    registered_message.envelope.kind == MessageKind::kData &&
                    registered_message.envelope.sequence == 0,
                "register response violated control contract");

        ControlRequest setup;
        setup.mutable_setup()->set_unit_id("asr.primary");
        auto expired_setup = make_request("setup-expired", "session-1", "", setup);
        expired_setup.envelope.created_monotonic_ns = 0;
        send_request(client.get(), expired_setup, false);
        const auto expired_message = read_message(client.get());
        const auto expired_response = parse_control_response(expired_message);
        require(expired_response.has_error() &&
                    expired_response.request_message_id() == "setup-expired" &&
                    expired_response.error().code() == static_cast<std::uint32_t>(
                        cabinflow::gateway::ControlErrorCode::kDeadlineExceeded) &&
                    expired_message.envelope.kind == MessageKind::kError &&
                    expired_message.envelope.work_id.empty(),
                "expired setup returns a correlated error without creating work");

        send_request(client.get(), make_request("setup-1", "session-1", "", setup), false);
        const auto setup_message = read_message(client.get());
        const auto setup_response = parse_control_response(setup_message);
        require(setup_response.has_setup() &&
                    setup_response.request_message_id() == "setup-1" &&
                    !setup_response.setup().work().work_id().empty() &&
                    setup_message.envelope.work_id ==
                        setup_response.setup().work().work_id(),
                "setup response did not return and envelope the created work id");
        const auto work_id = setup_message.envelope.work_id;

        send_request(client.get(), make_request("schema-1", "session-1", "", setup, 2),
                     false);
        const auto schema_message = read_message(client.get());
        const auto schema_response = parse_control_response(schema_message);
        require(schema_response.has_error() &&
                    schema_response.request_message_id() == "schema-1" &&
                    schema_message.envelope.schema_version ==
                        cabinflow::protocol::kCurrentSchemaVersion &&
                    schema_message.envelope.kind == MessageKind::kError &&
                    schema_message.envelope.work_id.empty(),
                "unsupported schema must produce a current-version error response");
        expect_peer_closed(client.get());

        FileDescriptor query_client = connect_client(gateway.port);
        ControlRequest query;
        query.mutable_task_info()->set_scope(
            cabinflow::protocol::v1::TASK_INFO_SCOPE_WORK);
        send_request(query_client.get(),
                     make_request("query-1", "session-1", work_id, query), false);
        const auto query_message = read_message(query_client.get());
        const auto query_response = parse_control_response(query_message);
        require(query_response.has_task_info() &&
                    query_response.task_info().works_size() == 1 &&
                    query_response.task_info().works(0).work_id() == work_id &&
                    query_response.task_info().works(0).state() ==
                        cabinflow::protocol::v1::WORK_STATE_RUNNING,
                "connection close must not cancel an existing work");

        FileDescriptor malformed_client = connect_client(gateway.port);
        std::string malformed_frame;
        malformed_frame.push_back('\0');
        malformed_frame.push_back('\0');
        malformed_frame.push_back('\0');
        malformed_frame.push_back('\x03');
        malformed_frame.append("bad");
        send_all(malformed_client.get(), malformed_frame);
        expect_peer_closed(malformed_client.get());

        stop_gateway(loop_thread, &gateway);
    } catch (...) {
        stop_gateway(loop_thread, &gateway);
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_control_gateway_over_tcp();
        std::cout << "control gateway tcp test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "control gateway tcp test failed: " << error.what() << '\n';
        return 1;
    }
}
