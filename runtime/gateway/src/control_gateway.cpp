#include <cabinflow/gateway/control_gateway.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <limits>
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
#include <cabinflow/gateway/message_limits.hpp>
#include <cabinflow/protocol/message_codec.hpp>
#include "grpc_connection.hpp"
#include <chrono>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>

#include "message_id.hpp"

namespace cabinflow::gateway {
namespace {

constexpr std::string_view kDataOutputIdPrefix = "data-output-";

struct TaskReservationGuard {
    const DataTaskHooks* hooks{nullptr};
    const protocol::Message* request{nullptr};
    ~TaskReservationGuard() {
        if (hooks) hooks->rollback(*request);
    }
    void committed() noexcept { hooks = nullptr; }
};

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
        case ControlValidationError::kExpired:
            response_error->set_code(
                static_cast<std::uint32_t>(ControlErrorCode::kDeadlineExceeded));
            response_error->set_message("control request deadline exceeded");
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

[[nodiscard]] bool data_deadline_expired(
    const protocol::MessageEnvelope& envelope,
    const runtime::Clock& clock) noexcept {
    constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
    const auto now = clock.now_monotonic_ns();
    if (now < envelope.created_monotonic_ns) {
        return false;
    }
    return now - envelope.created_monotonic_ns >=
           static_cast<std::uint64_t>(envelope.ttl_ms) *
               kNanosecondsPerMillisecond;
}

[[nodiscard]] protocol::v1::DeliveryErrorCode to_delivery_error_code(
    runtime::Runtime::TargetDeliveryFailure failure) {
    switch (failure) {
        case runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded:
            return protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED;
        case runtime::Runtime::TargetDeliveryFailure::kSessionCancelled:
            return protocol::v1::DELIVERY_ERROR_SESSION_CANCELLED;
        case runtime::Runtime::TargetDeliveryFailure::kWorkCancelled:
            return protocol::v1::DELIVERY_ERROR_WORK_CANCELLED;
        case runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic:
            return protocol::v1::DELIVERY_ERROR_UNSUPPORTED_TOPIC;
        case runtime::Runtime::TargetDeliveryFailure::kInvalidPayload:
            return protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD;
    }
    return protocol::v1::DELIVERY_ERROR_UNSPECIFIED;
}

[[nodiscard]] std::string_view to_delivery_error_detail(
    runtime::Runtime::TargetDeliveryFailure failure) noexcept {
    switch (failure) {
        case runtime::Runtime::TargetDeliveryFailure::kDeadlineExceeded:
            return "message deadline exceeded before handling";
        case runtime::Runtime::TargetDeliveryFailure::kSessionCancelled:
            return "session cancelled before handling";
        case runtime::Runtime::TargetDeliveryFailure::kWorkCancelled:
            return "work cancelled before handling";
        case runtime::Runtime::TargetDeliveryFailure::kUnsupportedTopic:
            return "target does not support this topic";
        case runtime::Runtime::TargetDeliveryFailure::kInvalidPayload:
            return "target rejected the payload";
    }
    return "unknown target delivery failure";
}

}  // namespace

struct DeliveryErrorResponder final {
    DeliveryErrorResponder(const runtime::Clock& clock, std::uint32_t response_ttl_ms)
        : builder(clock, sequencer, response_ttl_ms) {}

    void start_accepting() noexcept { accepting.store(true); }
    void stop_accepting() noexcept { accepting.store(false); }

    void send(const GrpcConnectionPtr& connection,
              const protocol::Message& request,
              protocol::v1::DeliveryErrorCode code,
              std::string_view detail,
              bool close_after_response = false) const {
        std::lock_guard<std::mutex> response_lock(connection->response_mutex);
        if (!accepting.load() || !connection->connected()) {
            return;
        }
        const auto response = builder.build(request, code, detail, connection->name());
        if (!response) {
            connection->force_close();
            return;
        }
        const auto frame = protocol::encode_message(response.message);
        if (!frame || frame.bytes.size() > kMaxMessageBytes) {
            connection->force_close();
            return;
        }

        static_cast<void>(connection->send(response.message));
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
    };

    struct PendingOutputs {
        struct Destination {
            std::weak_ptr<GrpcConnection> connection;
            std::string connection_name;
        };

        std::mutex mutex;
        std::map<std::pair<std::string, std::string>, Destination> destinations;
    };

    Impl(std::string name, std::string listen_ip, std::uint16_t listen_port,
         runtime::UnitRegistry& registry, runtime::Runtime& runtime,
         const runtime::Clock& clock, std::uint32_t response_ttl_ms)
        : registry(registry), runtime(runtime), clock(clock), response_ttl_ms(response_ttl_ms),
          delivery_responder(std::make_shared<DeliveryErrorResponder>(clock, response_ttl_ms)),
          validator(clock), service(registry),
          response_builder(clock, delivery_responder->sequencer, response_ttl_ms),
          listen_address(listen_ip + ":" + std::to_string(listen_port)) {
        if ((listen_ip != "127.0.0.1" && listen_ip != "::1") || name.empty())
            throw std::invalid_argument("gRPC demo requires explicit loopback IP and gateway name");
        if (listen_ip == "::1") listen_address = "[::1]:" + std::to_string(listen_port);
        grpc_service = std::make_unique<GrpcService>(std::move(name),
            [this](const GrpcConnectionPtr& connection, const protocol::Message& message) {
                handle_message(connection, message);
            },
            [this](const GrpcConnectionPtr& connection) { handle_connection(connection); });
    }

    ~Impl() {
        output_accepting.store(false);
        delivery_responder->stop_accepting();
    }

    void handle_connection(const GrpcConnectionPtr& connection) {
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            if (connection->connected()) {
                connections.emplace(connection->name(),
                                    std::make_shared<ConnectionState>());
                return;
            }
            connections.erase(connection->name());
        }
        // 断连只移除该连接的输出去向，不拥有 work，也不触发 cancel 或 exit。
        std::lock_guard<std::mutex> lock(pending_outputs->mutex);
        for (auto it = pending_outputs->destinations.begin();
             it != pending_outputs->destinations.end();) {
            if (it->second.connection_name == connection->name()) {
                it = pending_outputs->destinations.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] bool send_data_output(
        const protocol::MessageEnvelope& request, std::string_view topic,
        std::string payload, protocol::MessageKind kind) {
        if (!output_accepting.load() ||
            request.session_id.empty() || request.message_id.empty() ||
            topic.empty() || response_ttl_ms == 0U ||
            (kind != protocol::MessageKind::kData && kind != protocol::MessageKind::kError)) {
            return false;
        }

        const auto key = std::make_pair(request.session_id, request.message_id);
        std::lock_guard<std::mutex> lock(pending_outputs->mutex);
        const auto found = pending_outputs->destinations.find(key);
        if (found == pending_outputs->destinations.end()) {
            return false;
        }
        const auto connection = found->second.connection.lock();
        if (!connection || !connection->connected()) {
            pending_outputs->destinations.erase(found);
            return false;
        }

        std::lock_guard<std::mutex> response_lock(connection->response_mutex);

        const auto sequence = delivery_responder->sequencer.next(
            request.session_id, request.work_id, topic, connection->name());
        const auto message_id = generate_gateway_message_id(kDataOutputIdPrefix);
        if (!sequence || message_id.empty()) {
            pending_outputs->destinations.erase(found);
            return false;
        }

        protocol::Message response;
        auto& envelope = response.envelope;
        envelope.schema_version = protocol::kCurrentSchemaVersion;
        envelope.message_id = message_id;
        envelope.trace_id = request.trace_id;
        envelope.session_id = request.session_id;
        envelope.work_id = request.work_id;
        envelope.source_node = request.target_node;
        envelope.target_node = request.source_node;
        envelope.topic = std::string(topic);
        envelope.kind = kind;
        envelope.seat = request.seat;
        envelope.sequence = sequence.sequence;
        envelope.created_monotonic_ns = clock.now_monotonic_ns();
        envelope.ttl_ms = response_ttl_ms;
        envelope.is_final = true;
        response.payload = std::move(payload);
        const auto frame = protocol::encode_message(response);
        pending_outputs->destinations.erase(found);
        if (!frame || frame.bytes.size() > kMaxMessageBytes) {
            return false;
        }
        return connection->send(response);
    }

    [[nodiscard]] std::shared_ptr<ConnectionState> find_connection_state(
        const GrpcConnectionPtr& connection) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        const auto found = connections.find(connection->name());
        return found == connections.end() ? nullptr : found->second;
    }

    void send_encoded_response(const GrpcConnectionPtr& connection,
                               const protocol::Message& response,
                               bool close_after_response) {
        const auto frame = protocol::encode_message(response);
        if (!frame || frame.bytes.size() > kMaxMessageBytes) {
            connection->force_close();
            return;
        }

        static_cast<void>(connection->send(response));
        if (close_after_response) {
            // 在错误响应的写反应完成后结束流，不能与在途写并发 Finish。
            connection->shutdown();
        }
    }

    void send_response(const GrpcConnectionPtr& connection,
                       const ControlResponseBuildResult& response,
                       bool close_after_response) {
        if (!response) {
            connection->force_close();
            return;
        }
        send_encoded_response(connection, response.message, close_after_response);
    }

    void process_control_message(const GrpcConnectionPtr& connection,
                                 const protocol::Message& message) {
        std::lock_guard<std::mutex> response_lock(connection->response_mutex);
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

        const auto exit_work = validation.request->command.has_exit()
            ? registry.find_work(message.envelope.session_id, message.envelope.work_id)
            : runtime::WorkResult{runtime::UnitRegistryError::kWorkNotFound, {}};
        if (task_hooks.cancel && validation.request->command.has_exit() &&
            !validation.request->command.exit().reason().empty() && exit_work &&
            exit_work.work.state != runtime::WorkState::kExited) {
            // 取消与 Agent 的终态提交先在同一锁线性化，不能只在 Registry 写 exited 后再通知。
            task_hooks.cancel(message.envelope);
        }
        const auto service_result = service.handle(*validation.request);
        if (service_result.response.has_exit()) {
            // 先使 Ledger 和 worker 看见取消，再把 Exit 成功响应发给客户端。
            runtime.cancel_work(validation.request->message.envelope.session_id,
                                validation.request->message.envelope.work_id);
        }
        const auto response = response_builder.build(*validation.request, service_result,
                                                     connection->name());
        send_response(connection, response, false);
    }

    void process_data_message(const GrpcConnectionPtr& connection,
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

        if (data_deadline_expired(validation.request->message.envelope, clock)) {
            // 解码后先判断时效，再查找目标；已过期消息不消耗目标队列容量。
            delivery_responder->send(
                connection, validation.request->message,
                protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED,
                "data message deadline exceeded");
            return;
        }

        const auto& envelope = validation.request->message.envelope;
        const auto work = registry.find_work(envelope.session_id, envelope.work_id);
        if (!work) {
            delivery_responder->send(
                connection, validation.request->message,
                protocol::v1::DELIVERY_ERROR_WORK_NOT_FOUND,
                "work does not exist in this session");
            return;
        }
        if (work.work.state == runtime::WorkState::kExited) {
            delivery_responder->send(
                connection, validation.request->message,
                protocol::v1::DELIVERY_ERROR_WORK_CANCELLED,
                "work has exited");
            return;
        }

        if (envelope.target_node != work.work.unit_id) {
            // 只限制外部入口；内部 Runtime 投递仍可沿同一 work 跨 Target。
            delivery_responder->send(
                connection, validation.request->message,
                protocol::v1::DELIVERY_ERROR_INVALID_ENVELOPE,
                "target_node does not match work unit_id");
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
        TaskReservationGuard task_slot;
        if (task_hooks.try_reserve) {
            if (!task_hooks.try_reserve(request)) {
                delivery_responder->send(connection, request,
                    protocol::v1::DELIVERY_ERROR_QUEUE_FULL,
                    "application task slot is busy or admission is closed");
                return;
            }
            task_slot.hooks = &task_hooks;
            task_slot.request = &request;
        }
        const bool managed_task = static_cast<bool>(task_hooks.complete);
        // 非法 final 也需要在业务结束并清理后得到唯一失败终态；普通分片仍无输出去向。
        const bool expects_output = managed_task || request.envelope.is_final;
        const auto output_key = std::make_pair(request.envelope.session_id,
                                               request.envelope.message_id);
        bool pending_inserted = false;
        if (expects_output) {
            std::lock_guard<std::mutex> lock(pending_outputs->mutex);
            pending_inserted = pending_outputs->destinations.emplace(
                output_key, PendingOutputs::Destination{connection,
                                                        connection->name()}).second;
        }
        if (expects_output && !pending_inserted) {
            delivery_responder->send(
                connection, request,
                protocol::v1::DELIVERY_ERROR_DUPLICATE_MESSAGE,
                "final input message ID already has a pending destination");
            return;
        }
        const std::weak_ptr<GrpcConnection> weak_connection = connection;
        const std::weak_ptr<DeliveryErrorResponder> weak_responder = delivery_responder;
        const auto task_failure = std::make_shared<
            std::optional<runtime::Runtime::TargetDeliveryFailure>>();
        auto failure_handler = [weak_responder, weak_connection, request,
                                managed_task, task_failure](
                                   runtime::Runtime::TargetDeliveryFailure failure) {
            if (managed_task) {
                // 同一 worker 先记录 failure，再执行 completion；业务终态只发送一次。
                *task_failure = failure;
                return;
            }
            const auto responder = weak_responder.lock();
            const auto failed_connection = weak_connection.lock();
            if (!responder || !failed_connection) {
                return;
            }
            responder->send(
                failed_connection, request, to_delivery_error_code(failure),
                to_delivery_error_detail(failure));
        };

        const std::weak_ptr<PendingOutputs> weak_pending = pending_outputs;
        auto erase_pending = [weak_pending, output_key, pending_inserted] {
            if (!pending_inserted) {
                return;
            }
            if (const auto pending = weak_pending.lock()) {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->destinations.erase(output_key);
            }
        };

        auto completion_handler = [this, request, managed_task, task_failure, erase_pending] {
            if (managed_task) {
                const auto output = task_hooks.complete(request, *task_failure);
                static_cast<void>(send_data_output(request.envelope, output.topic,
                                                  output.payload, output.kind));
            }
            erase_pending();
        };

        const auto admission = runtime.admit_reserved(
            std::move(reservation.reservation), validation.request->message,
            std::move(failure_handler), completion_handler);
        if (!admission.admitted) {
            // 未进入 Runtime 的请求只撤销占位，不能冒充 handler 已经完成。
            erase_pending();
            delivery_responder->send(
                connection, request, to_delivery_error_code(admission.delivery_result),
                "data message rejected by session ledger");
        } else {
            task_slot.committed();
        }
    }

    void process_message(const GrpcConnectionPtr& connection,
                         const protocol::Message& message) {
        if (message.envelope.topic == "control.request") {
            process_control_message(connection, message);
            return;
        }
        process_data_message(connection, message);
    }

    void handle_message(const GrpcConnectionPtr& connection, const protocol::Message& message) {
        const auto state = find_connection_state(connection);
        if (!state) { connection->force_close(); return; }
        // 只串行化同一流的准入；不持有 reactor 锁进入业务回调。
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!admission_accepting.load()) { connection->shutdown(); return; }
        process_message(connection, message);
    }

    std::mutex connections_mutex;
    std::unordered_map<std::string, std::shared_ptr<ConnectionState>> connections;
    runtime::UnitRegistry& registry;
    runtime::Runtime& runtime;
    const runtime::Clock& clock;
    std::uint32_t response_ttl_ms;
    std::atomic<bool> output_accepting{true};
    std::shared_ptr<PendingOutputs> pending_outputs{std::make_shared<PendingOutputs>()};
    std::shared_ptr<DeliveryErrorResponder> delivery_responder;
    ControlEnvelopeValidator validator;
    DataEnvelopeValidator data_validator;
    ControlService service;
    DataTaskHooks task_hooks;
    bool started{false};
    ControlResponseBuilder response_builder;
    std::atomic<bool> admission_accepting{false};
    std::string listen_address;
    int port{0};
    std::unique_ptr<GrpcService> grpc_service;
    std::unique_ptr<grpc::Server> server;
};

ControlGateway::ControlGateway(std::string name, std::string listen_ip,
                               std::uint16_t listen_port, runtime::UnitRegistry& registry,
                               runtime::Runtime& runtime, const runtime::Clock& clock,
                               std::uint32_t response_ttl_ms)
    : impl_(std::make_unique<Impl>(std::move(name), std::move(listen_ip), listen_port,
                                   registry, runtime, clock, response_ttl_ms)) {}
ControlGateway::~ControlGateway() { stop(); }

void ControlGateway::set_data_task_hooks(DataTaskHooks hooks) {
    if (impl_->started || !hooks.try_reserve || !hooks.rollback ||
        !hooks.complete || !hooks.cancel) {
        throw std::invalid_argument("complete data task hooks must be configured before start");
    }
    impl_->task_hooks = std::move(hooks);
}

void ControlGateway::start() {
    if (impl_->started) throw std::logic_error("gRPC gateway is a one-shot instance");
    impl_->started = true;
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(static_cast<int>(kMaxMessageBytes));
    builder.SetMaxSendMessageSize(static_cast<int>(kMaxMessageBytes));
    builder.AddListeningPort(impl_->listen_address, grpc::InsecureServerCredentials(), &impl_->port);
    builder.RegisterService(impl_->grpc_service.get());
    impl_->server = builder.BuildAndStart();
    if (!impl_->server || impl_->port == 0) throw std::runtime_error("gRPC listen failed");
    impl_->output_accepting.store(true);
    impl_->delivery_responder->start_accepting();
    impl_->admission_accepting.store(true);
}
void ControlGateway::stop_admission() { impl_->admission_accepting.store(false); }
void ControlGateway::stop() {
    stop_admission();
    if (!impl_->server) return;
    impl_->output_accepting.store(false);
    impl_->delivery_responder->stop_accepting();
    // 期限只用于网络流收敛；调用方须在此之前完成 Runtime drain，不能强杀算法线程。
    impl_->server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    impl_->server->Wait();
    impl_->server.reset();
    std::lock_guard<std::mutex> lock(impl_->pending_outputs->mutex);
    impl_->pending_outputs->destinations.clear();
}
std::uint16_t ControlGateway::bound_port() const { return static_cast<std::uint16_t>(impl_->port); }

bool ControlGateway::send_data_output(
    const protocol::MessageEnvelope& request, std::string_view topic,
    std::string payload, protocol::MessageKind kind) {
    return impl_->send_data_output(request, topic, std::move(payload), kind);
}

bool ControlGateway::data_output_fits(
    const protocol::MessageEnvelope& request, const DataTaskOutput& output) {
    protocol::Message response;
    response.envelope = request;
    auto& envelope = response.envelope;
    envelope.message_id = std::string(kDataOutputIdPrefix) + std::string(32, '0');
    envelope.source_node = request.target_node;
    envelope.target_node = request.source_node;
    envelope.topic = output.topic;
    envelope.kind = output.kind;
    envelope.sequence = std::numeric_limits<std::uint64_t>::max();
    envelope.created_monotonic_ns = std::numeric_limits<std::uint64_t>::max();
    envelope.ttl_ms = std::numeric_limits<std::uint32_t>::max();
    envelope.is_final = true;
    response.payload = output.payload;
    const auto encoded = protocol::encode_message(response);
    return encoded && encoded.bytes.size() <= kMaxMessageBytes;
}

}  // namespace cabinflow::gateway
