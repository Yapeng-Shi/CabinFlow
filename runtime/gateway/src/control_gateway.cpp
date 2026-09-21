#include <cabinflow/gateway/control_gateway.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/gateway/control_response_builder.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/gateway/data_envelope_validator.hpp>
#include <cabinflow/gateway/delivery_error_builder.hpp>
#include <cabinflow/gateway/runtime_message_framer.hpp>
#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/net/tcp_connection.hpp>
#include <cabinflow/net/tcp_server.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>

namespace cabinflow::gateway {
namespace {

[[nodiscard]] ControlServiceResult make_validation_error(
    ControlValidationError error) {
    ControlServiceResult result;
    auto* response_error = result.response.mutable_error();
    switch (error) {
        case ControlValidationError::kUnsupportedSchemaVersion:
            response_error->set_code(
                static_cast<std::uint32_t>(ControlErrorCode::kUnsupportedSchemaVersion));
            response_error->set_message("unsupported schema version");
            break;
        case ControlValidationError::kInvalidEnvelope:
        case ControlValidationError::kInvalidControlIdentity:
            response_error->set_code(
                static_cast<std::uint32_t>(ControlErrorCode::kInvalidEnvelope));
            response_error->set_message("invalid control envelope identity");
            break;
        case ControlValidationError::kMalformedControlPayload:
        case ControlValidationError::kNone:
            response_error->set_code(static_cast<std::uint32_t>(ControlErrorCode::kInternal));
            response_error->set_message("unexpected gateway validation error");
            break;
    }
    return result;
}

[[nodiscard]] protocol::v1::DeliveryErrorCode to_delivery_error_code(
    runtime::DeliveryResult result) {
    switch (result) {
        case runtime::DeliveryResult::kInvalidEnvelope:
            return protocol::v1::DELIVERY_ERROR_INVALID_ENVELOPE;
        case runtime::DeliveryResult::kUnsupportedSchemaVersion:
            return protocol::v1::DELIVERY_ERROR_UNSUPPORTED_SCHEMA;
        case runtime::DeliveryResult::kExpired:
            return protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED;
        case runtime::DeliveryResult::kDuplicateMessage:
            return protocol::v1::DELIVERY_ERROR_DUPLICATE_MESSAGE;
        case runtime::DeliveryResult::kStaleSequence:
            return protocol::v1::DELIVERY_ERROR_STALE_SEQUENCE;
        case runtime::DeliveryResult::kStreamFinalized:
            return protocol::v1::DELIVERY_ERROR_STREAM_FINALIZED;
        case runtime::DeliveryResult::kSessionCancelled:
            return protocol::v1::DELIVERY_ERROR_SESSION_CANCELLED;
        case runtime::DeliveryResult::kWorkCancelled:
            return protocol::v1::DELIVERY_ERROR_WORK_CANCELLED;
        case runtime::DeliveryResult::kAccepted:
            break;
    }
    return protocol::v1::DELIVERY_ERROR_UNSPECIFIED;
}

[[nodiscard]] protocol::v1::DeliveryErrorCode to_delivery_error_code(
    runtime::Runtime::TargetDeliveryFailure failure) {
    switch (failure) {
        case runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded:
            return protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED;
        case runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic:
            return protocol::v1::DELIVERY_ERROR_UNSUPPORTED_TOPIC;
        case runtime::Runtime::TargetDeliveryFailure::kInvalidPayload:
            return protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD;
    }
    return protocol::v1::DELIVERY_ERROR_UNSPECIFIED;
}

}  // namespace

struct DeliveryErrorResponder final {
    DeliveryErrorResponder(const runtime::Clock& clock, std::uint32_t response_ttl_ms)
        : builder(clock, sequencer, response_ttl_ms) {}

    void start_accepting() noexcept { accepting.store(true); }
    void stop_accepting() noexcept { accepting.store(false); }

    void send(const net::TcpConnectionPtr& connection,
              const protocol::Message& request,
              protocol::v1::DeliveryErrorCode code,
              std::string_view detail,
              bool close_after_response = false) const {
        if (!accepting.load() || !connection->connected()) {
            return;
        }
        const auto response = builder.build(request, code, detail, connection->name());
        if (!response) {
            connection->force_close();
            return;
        }
        const auto frame = RuntimeMessageFramer::encode(response.message);
        if (!frame) {
            connection->force_close();
            return;
        }

        try {
            connection->send(frame.bytes);
        } catch (const std::logic_error&) {
            // 连接可能恰好在 worker 回报失败时关闭；此时没有安全的响应接收方。
            return;
        }
        if (close_after_response) {
            connection->shutdown();
        }
    }

    std::atomic<bool> accepting{true};
    ResponseSequencer sequencer;
    DeliveryErrorBuilder builder;
};

struct ControlGateway::Impl {
    struct ConnectionState {
        std::mutex mutex;
        RuntimeMessageFramer framer;
    };

    Impl(net::EventLoop& loop_arg, std::string name, std::string listen_ip,
         std::uint16_t listen_port, runtime::UnitRegistry& registry,
         runtime::Runtime& runtime,
         const runtime::Clock& clock, std::uint32_t response_ttl_ms)
        : runtime(runtime),
          delivery_responder(std::make_shared<DeliveryErrorResponder>(
              clock, response_ttl_ms)),
          service(registry),
          response_builder(clock, delivery_responder->sequencer, response_ttl_ms),
          server(loop_arg, std::move(name), std::move(listen_ip), listen_port) {
        server.set_connection_callback(
            [this](const net::TcpConnectionPtr& connection) {
                handle_connection(connection);
            });
        server.set_message_callback(
            [this](const net::TcpConnectionPtr& connection, std::string_view bytes) {
                handle_message(connection, bytes);
            });
    }

    ~Impl() { delivery_responder->stop_accepting(); }

    void handle_connection(const net::TcpConnectionPtr& connection) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        if (connection->connected()) {
            connections.emplace(connection->name(), std::make_shared<ConnectionState>());
            return;
        }
        // 连接关闭只释放协议缓存；不拥有 work，因此不触发 cancel 或 exit。
        connections.erase(connection->name());
    }

    [[nodiscard]] std::shared_ptr<ConnectionState> find_connection_state(
        const net::TcpConnectionPtr& connection) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        const auto found = connections.find(connection->name());
        return found == connections.end() ? nullptr : found->second;
    }

    void send_encoded_response(const net::TcpConnectionPtr& connection,
                               const protocol::Message& response,
                               bool close_after_response) {
        const auto frame = RuntimeMessageFramer::encode(response);
        if (!frame) {
            connection->force_close();
            return;
        }

        connection->send(frame.bytes);
        if (close_after_response) {
            // TcpConnection 会在既有发送缓冲写尽后 half-close，避免丢失 schema 错误。
            connection->shutdown();
        }
    }

    void send_response(const net::TcpConnectionPtr& connection,
                       const ControlResponseBuildResult& response,
                       bool close_after_response) {
        if (!response) {
            connection->force_close();
            return;
        }
        send_encoded_response(connection, response.message, close_after_response);
    }

    void process_control_message(const net::TcpConnectionPtr& connection,
                                 const protocol::Message& message) {
        auto validation = validator.validate(message);
        if (!validation) {
            if (!validation.can_return_error || !validation.request.has_value()) {
                connection->force_close();
                return;
            }
            const auto response = response_builder.build(
                *validation.request, make_validation_error(validation.error),
                connection->name());
            send_response(connection, response, validation.should_close_connection);
            return;
        }

        const auto service_result = service.handle(*validation.request);
        const auto response = response_builder.build(*validation.request, service_result,
                                                     connection->name());
        send_response(connection, response, false);
    }

    void process_data_message(const net::TcpConnectionPtr& connection,
                              const protocol::Message& message) {
        const auto validation = data_validator.validate(message);
        if (!validation) {
            if (!validation.can_return_error || !validation.request.has_value()) {
                connection->force_close();
                return;
            }
            const auto code = validation.error ==
                                      DataValidationError::kUnsupportedSchemaVersion
                                  ? protocol::v1::DELIVERY_ERROR_UNSUPPORTED_SCHEMA
                                  : protocol::v1::DELIVERY_ERROR_INVALID_ENVELOPE;
            delivery_responder->send(connection, validation.request->message, code,
                                     "invalid data envelope",
                                     validation.should_close_connection);
            return;
        }

        auto reservation = runtime.reserve_target(
            validation.request->message.envelope.target_node);
        if (!reservation) {
            if (reservation.error ==
                runtime::Runtime::TargetReservationError::kNotRunning) {
                connection->force_close();
                return;
            }
            const auto code = reservation.error ==
                                      runtime::Runtime::TargetReservationError::kUnknownTarget
                                  ? protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET
                                  : protocol::v1::DELIVERY_ERROR_QUEUE_FULL;
            delivery_responder->send(
                connection, validation.request->message, code,
                code == protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET
                    ? "unknown target node"
                    : "target queue is full");
            return;
        }

        const protocol::Message request = validation.request->message;
        const std::weak_ptr<net::TcpConnection> weak_connection = connection;
        const std::weak_ptr<DeliveryErrorResponder> weak_responder = delivery_responder;
        auto failure_handler = [weak_responder, weak_connection, request](
                                   runtime::Runtime::TargetDeliveryFailure failure) {
            const auto responder = weak_responder.lock();
            const auto failed_connection = weak_connection.lock();
            if (!responder || !failed_connection) {
                return;
            }
            responder->send(
                failed_connection, request, to_delivery_error_code(failure),
                failure == runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded
                    ? "message deadline exceeded before handling"
                    : "target rejected data message");
        };

        const auto admission = runtime.admit_reserved(
            std::move(reservation.reservation), validation.request->message,
            std::move(failure_handler));
        if (!admission.admitted) {
            delivery_responder->send(
                connection, request, to_delivery_error_code(admission.delivery_result),
                "data message rejected by session ledger");
        }
    }

    void process_message(const net::TcpConnectionPtr& connection,
                         const protocol::Message& message) {
        if (message.envelope.topic == "control.request") {
            process_control_message(connection, message);
            return;
        }
        process_data_message(connection, message);
    }

    void handle_message(const net::TcpConnectionPtr& connection,
                        std::string_view bytes) {
        const auto connection_state = find_connection_state(connection);
        if (!connection_state) {
            connection->force_close();
            return;
        }

        std::lock_guard<std::mutex> lock(connection_state->mutex);
        const auto framed = connection_state->framer.feed(bytes);
        for (const auto& message : framed.messages) {
            process_message(connection, message);
        }
        if (!framed) {
            // 非法长度或 RuntimeMessage Protobuf 损坏没有可信关联身份，直接关闭。
            connection->force_close();
        }
    }

    std::mutex connections_mutex;
    std::unordered_map<std::string, std::shared_ptr<ConnectionState>> connections;
    runtime::Runtime& runtime;
    std::shared_ptr<DeliveryErrorResponder> delivery_responder;
    ControlEnvelopeValidator validator;
    DataEnvelopeValidator data_validator;
    ControlService service;
    ControlResponseBuilder response_builder;
    net::TcpServer server;
};

ControlGateway::ControlGateway(net::EventLoop& loop, std::string name,
                               std::string listen_ip, std::uint16_t listen_port,
                               runtime::UnitRegistry& registry,
                               runtime::Runtime& runtime,
                               const runtime::Clock& clock,
                               std::uint32_t response_ttl_ms)
    : impl_(std::make_unique<Impl>(loop, std::move(name), std::move(listen_ip),
                                   listen_port, registry, runtime, clock,
                                   response_ttl_ms)) {}

ControlGateway::~ControlGateway() = default;

void ControlGateway::set_worker_count(std::size_t worker_count) {
    impl_->server.set_worker_count(worker_count);
}

void ControlGateway::start() {
    impl_->delivery_responder->start_accepting();
    impl_->server.start();
}

void ControlGateway::stop() {
    impl_->delivery_responder->stop_accepting();
    impl_->server.stop();
}

std::uint16_t ControlGateway::bound_port() const {
    return impl_->server.bound_port();
}

}  // namespace cabinflow::gateway
