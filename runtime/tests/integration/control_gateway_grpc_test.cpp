#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <control.pb.h>

#include <cabinflow/gateway/control_gateway.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include "grpc_test_client.hpp"
#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

namespace {

using cabinflow::test::GrpcTestClient;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
using cabinflow::protocol::v1::ControlRequest;
using cabinflow::protocol::v1::ControlResponse;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class NullLogger final : public cabinflow::observability::Logger {
public:
    void log(const cabinflow::observability::Event&) override {}
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

GatewayHandle start_gateway() {
    GatewayHandle handle;
    handle.registry = std::make_unique<cabinflow::runtime::UnitRegistry>();
    handle.clock = std::make_unique<cabinflow::runtime::SteadyClock>();
    handle.transport = std::make_unique<cabinflow::transport::InMemoryTransport>();
    handle.logger = std::make_unique<NullLogger>();
    handle.runtime = std::make_unique<cabinflow::runtime::Runtime>(
        *handle.transport, *handle.clock, *handle.logger);
    handle.gateway = std::make_unique<cabinflow::gateway::ControlGateway>(
        "control-gateway-test", "127.0.0.1", 0,
        *handle.registry, *handle.runtime, *handle.clock, 500);
    handle.gateway->start();
    handle.port = handle.gateway->bound_port();
    return handle;
}

void stop_gateway(GatewayHandle* handle) {
    if (!handle->gateway) return;
    handle->runtime->stop();
    handle->gateway->stop();
    handle->gateway.reset();
}

GrpcTestClient connect_client(std::uint16_t port) { return GrpcTestClient(port); }
Message read_message(GrpcTestClient& client) { return client.read(); }
void expect_peer_closed(GrpcTestClient& client) {
    static_cast<void>(client.wait_closed());
}

Message make_request(std::string message_id, std::string session_id,
                     std::string work_id, const ControlRequest& request,
                     std::uint32_t schema_version =
                         cabinflow::protocol::kCurrentSchemaVersion) {
    Message message;
    message.envelope.schema_version = schema_version;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-grpc";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "grpc-client";
    message.envelope.target_node = "control-gateway";
    message.envelope.topic = "control.request";
    message.envelope.kind = MessageKind::kData;
    message.envelope.created_monotonic_ns =
        cabinflow::runtime::SteadyClock{}.now_monotonic_ns();
    message.envelope.ttl_ms = 1000;
    require(request.SerializeToString(&message.payload), "serialize request payload");
    return message;
}

void send_request(GrpcTestClient& client, const Message& request) { client.send(request); }

ControlResponse parse_control_response(const Message& message) {
    require(message.envelope.topic == "control.response", "response topic changed");
    ControlResponse response;
    require(response.ParseFromString(message.payload), "decode response payload");
    return response;
}

void test_control_gateway_over_grpc() {
    GatewayHandle gateway = start_gateway();
    try {
        GrpcTestClient client = connect_client(gateway.port);

        ControlRequest registration;
        registration.mutable_register_unit()->set_unit_id("asr.primary");
        registration.mutable_register_unit()->add_capabilities("speech");
        registration.mutable_register_unit()->set_max_concurrent_work(1);
        send_request(client, make_request("register-1", "", "", registration));
        const auto registered_message = read_message(client);
        const auto registered = parse_control_response(registered_message);
        require(registered.has_register_unit() &&
                    registered.request_message_id() == "register-1" &&
                    registered_message.envelope.kind == MessageKind::kData &&
                    registered_message.envelope.sequence == 0,
                "register response violated control contract");

        ControlRequest setup;
        setup.mutable_setup()->set_unit_id("asr.primary");
        for (const std::uint32_t version : {65537U, 0xffffffffU}) {
            GrpcTestClient unknown = connect_client(gateway.port);
            const auto id = "unsupported-" + std::to_string(version);
            send_request(unknown, make_request(id, "session-1", "", setup, version));
            const auto error_message = read_message(unknown);
            const auto error = parse_control_response(error_message);
            require(error.has_error() && error.request_message_id() == id &&
                    error.error().code() == static_cast<std::uint32_t>(
                        cabinflow::gateway::ControlErrorCode::kUnsupportedSchemaVersion) &&
                    error_message.envelope.schema_version == cabinflow::protocol::kCurrentSchemaVersion &&
                    error_message.envelope.kind == MessageKind::kError && error_message.envelope.work_id.empty(),
                    "full uint32 unsupported schema returns correlated current-schema error");
            expect_peer_closed(unknown);
        }
        // max capacity 为 1；后续正常 Setup 成功同时证明未知版本未创建 work/占用容量。
        auto expired_setup = make_request("setup-expired", "session-1", "", setup);
        expired_setup.envelope.created_monotonic_ns = 0;
        send_request(client, expired_setup);
        const auto expired_message = read_message(client);
        const auto expired_response = parse_control_response(expired_message);
        require(expired_response.has_error() &&
                    expired_response.request_message_id() == "setup-expired" &&
                    expired_response.error().code() == static_cast<std::uint32_t>(
                        cabinflow::gateway::ControlErrorCode::kDeadlineExceeded) &&
                    expired_message.envelope.kind == MessageKind::kError &&
                    expired_message.envelope.work_id.empty(),
                "expired setup returns a correlated error without creating work");

        send_request(client, make_request("setup-1", "session-1", "", setup));
        const auto setup_message = read_message(client);
        const auto setup_response = parse_control_response(setup_message);
        require(setup_response.has_setup() &&
                    setup_response.request_message_id() == "setup-1" &&
                    !setup_response.setup().work().work_id().empty() &&
                    setup_message.envelope.work_id ==
                        setup_response.setup().work().work_id(),
                "setup response did not return and envelope the created work id");
        const auto work_id = setup_message.envelope.work_id;

        send_request(client, make_request("schema-1", "session-1", "", setup, 2));
        const auto schema_message = read_message(client);
        const auto schema_response = parse_control_response(schema_message);
        require(schema_response.has_error() &&
                    schema_response.request_message_id() == "schema-1" &&
                    schema_message.envelope.schema_version ==
                        cabinflow::protocol::kCurrentSchemaVersion &&
                    schema_message.envelope.kind == MessageKind::kError &&
                    schema_message.envelope.work_id.empty(),
                "unsupported schema must produce a current-version error response");
        expect_peer_closed(client);

        GrpcTestClient query_client = connect_client(gateway.port);
        ControlRequest query;
        query.mutable_task_info()->set_scope(
            cabinflow::protocol::v1::TASK_INFO_SCOPE_WORK);
        send_request(query_client,
                     make_request("query-1", "session-1", work_id, query));
        const auto query_message = read_message(query_client);
        const auto query_response = parse_control_response(query_message);
        require(query_response.has_task_info() &&
                    query_response.task_info().works_size() == 1 &&
                    query_response.task_info().works(0).work_id() == work_id &&
                    query_response.task_info().works(0).state() ==
                        cabinflow::protocol::v1::WORK_STATE_RUNNING,
                "connection close must not cancel an existing work");

        for (bool invalid_kind : {true, false}) {
            GrpcTestClient malformed_client = connect_client(gateway.port);
            const auto encoded = cabinflow::protocol::encode_message(
                make_request("malformed-query", "session-1", work_id, query));
            cabinflow::protocol::v1::RuntimeMessage malformed;
            require(encoded && malformed.ParseFromString(encoded.bytes), "valid baseline wire message");
            if (invalid_kind) malformed.set_kind(static_cast<cabinflow::protocol::v1::WireMessageKind>(99));
            else malformed.set_seat(static_cast<cabinflow::protocol::v1::WireSeatPosition>(99));
            malformed_client.send_wire(malformed);
            require(malformed_client.wait_closed().error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                    "malformed RuntimeMessage enum terminates stream without a business response");
        }

        GrpcTestClient oversized_client = connect_client(gateway.port);
        cabinflow::protocol::v1::RuntimeMessage oversized;
        oversized.set_payload(std::string(cabinflow::gateway::kMaxMessageBytes + 1, 'x'));
        oversized_client.send_wire(oversized);
        require(oversized_client.wait_closed().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED,
                "gRPC rejects an input larger than the public message limit");

        stop_gateway(&gateway);
    } catch (...) {
        stop_gateway(&gateway);
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_control_gateway_over_grpc();
        std::cout << "control gateway grpc test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "control gateway grpc test failed: " << error.what() << '\n';
        return 1;
    }
}
