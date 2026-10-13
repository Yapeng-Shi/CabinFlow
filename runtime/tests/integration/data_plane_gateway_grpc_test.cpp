#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cockpit_text.pb.h>
#include <control.pb.h>
#include <delivery.pb.h>

#include <cabinflow/agent/dialogue_text_node.hpp>
#include <cabinflow/gateway/control_gateway.hpp>
#include "grpc_test_client.hpp"
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/runtime/unit_registry.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "recording_logger.hpp"

namespace {

using namespace std::chrono_literals;
using cabinflow::test::GrpcTestClient;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

class OtherTarget final : public cabinflow::runtime::TargetNode {
public:
    explicit OtherTarget(std::atomic<int>& calls) : calls_(calls) {}
    std::string_view name() const noexcept override { return "other.primary"; }
    cabinflow::runtime::RuntimeError start(cabinflow::runtime::NodeContext&) override {
        return cabinflow::runtime::RuntimeError::kNone;
    }
    void stop() noexcept override {}
    cabinflow::runtime::MessageHandlingResult on_message(const Message&) noexcept override {
        ++calls_;
        return cabinflow::runtime::MessageHandlingResult::kHandled;
    }
private:
    std::atomic<int>& calls_;
};

class GatewayFixture final {
public:
    GatewayFixture()
        : runtime_(transport_, clock_, logger_) {
        gateway_ = std::make_unique<cabinflow::gateway::ControlGateway>(
            "data-gateway-test", "127.0.0.1", 0,
            registry_, runtime_, clock_, 500);

        require(runtime_.add_target_node(
                    std::make_unique<cabinflow::agent::DialogueTextNode>(
                        [this](const cabinflow::protocol::MessageEnvelope& request,
                               std::string_view topic, std::string payload) {
                            return gateway_->send_data_output(request, topic,
                                                              std::move(payload), MessageKind::kData);
                        }), 8) ==
                    cabinflow::runtime::RuntimeError::kNone,
                "dialogue target registers");
        require(runtime_.add_target_node(std::make_unique<OtherTarget>(other_calls_), 2) ==
                    cabinflow::runtime::RuntimeError::kNone, "second real target registers");
        require(runtime_.start() == cabinflow::runtime::RuntimeError::kNone,
                "runtime starts before gateway accepts data");
        gateway_->start();
        port_ = gateway_->bound_port();
    }

    ~GatewayFixture() {
        // worker 可能还在生成 DeliveryError，因此先停止 Runtime，再释放 Gateway。
        runtime_.stop();
        gateway_->stop();
        gateway_.reset();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] cabinflow::runtime::SteadyClock& clock() noexcept {
        return clock_;
    }
    [[nodiscard]] cabinflow::test::RecordingLogger& logger() noexcept {
        return logger_;
    }
    cabinflow::runtime::Runtime& runtime() noexcept { return runtime_; }
    int other_calls() const noexcept { return other_calls_.load(); }

private:
    cabinflow::runtime::SteadyClock clock_;
    cabinflow::transport::InMemoryTransport transport_;
    cabinflow::test::RecordingLogger logger_;
    std::atomic<int> other_calls_{0};
    cabinflow::runtime::Runtime runtime_;
    cabinflow::runtime::UnitRegistry registry_;
    std::unique_ptr<cabinflow::gateway::ControlGateway> gateway_;
    std::uint16_t port_{0};
};

GrpcTestClient connect_client(std::uint16_t port) { return GrpcTestClient(port); }
void send_message(GrpcTestClient& client, const Message& message) { client.send(message); }
Message read_message(GrpcTestClient& client) { return client.read(); }
void expect_no_response(GrpcTestClient& client) { client.no_response(); }

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

std::vector<std::string> dialogue_message_ids(
    const cabinflow::test::RecordingLogger& logger) {
    std::vector<std::string> ids;
    for (const auto& event : logger.snapshot()) {
        if (event.component == "dialogue" && event.name == "text_input_received") {
            ids.push_back(event.message_id);
        }
    }
    return ids;
}

Message make_control(std::string message_id, std::string session_id,
                     std::string work_id,
                     const cabinflow::protocol::v1::ControlRequest& request) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-control";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "grpc-client";
    message.envelope.target_node = "control-gateway";
    message.envelope.topic = "control.request";
    message.envelope.created_monotonic_ns =
        cabinflow::runtime::SteadyClock{}.now_monotonic_ns();
    message.envelope.ttl_ms = 1'000;
    require(request.SerializeToString(&message.payload), "serialize control request");
    return message;
}

std::string register_and_setup(GrpcTestClient& descriptor) {
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
    message.envelope.source_node = "grpc-client";
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

cabinflow::protocol::v1::DeliveryError read_delivery_error(GrpcTestClient& descriptor,
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

void test_typed_data_plane_over_grpc() {
    GatewayFixture fixture;
    auto client = connect_client(fixture.port());
    const auto work_id = register_and_setup(client);

    const auto now = fixture.clock().now_monotonic_ns();
    send_message(client, make_data("text-0", work_id, "dialogue.primary",
                                         "cockpit.text.input", 0, now, 1'000,
                                         text_payload("打")));
    wait_for_dialogue_event(fixture.logger(), 1);
    expect_no_response(client);

    send_message(client, make_data("text-bad-proto", work_id, "dialogue.primary",
                                         "cockpit.text.input", 1, now, 1'000,
                                         "not a TextInput protobuf"));
    read_delivery_error(client, "text-bad-proto",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client, make_data("text-empty", work_id, "dialogue.primary",
                                         "cockpit.text.input", 2, now, 1'000,
                                         text_payload("")));
    read_delivery_error(client, "text-empty",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client, make_data("text-invalid-utf8", work_id,
                                         "dialogue.primary", "cockpit.text.input", 3,
                                         now, 1'000, text_payload("\xc3\x28")));
    read_delivery_error(client, "text-invalid-utf8",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_PAYLOAD);

    send_message(client, make_data("unsupported-topic", work_id,
                                         "dialogue.primary", "cockpit.text.unsupported", 0,
                                         now, 1'000, text_payload("unsupported")));
    read_delivery_error(client, "unsupported-topic",
                        cabinflow::protocol::v1::DELIVERY_ERROR_UNSUPPORTED_TOPIC);

    send_message(client, make_data("unknown-target", work_id, "missing.target",
                                         "cockpit.text.unknown", 0, now, 1'000,
                                         text_payload("ignored")));
    read_delivery_error(client, "unknown-target",
                        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_ENVELOPE);

    // 已注册的 Unit 可以没有 Runtime Target；归属正确后才独立检验 UNKNOWN_TARGET。
    cabinflow::protocol::v1::ControlRequest missing_registration;
    missing_registration.mutable_register_unit()->set_unit_id("missing.target");
    missing_registration.mutable_register_unit()->add_capabilities("missing");
    missing_registration.mutable_register_unit()->set_max_concurrent_work(1);
    send_message(client, make_control("register-missing", "", "", missing_registration));
    cabinflow::protocol::v1::ControlResponse missing_response;
    require(missing_response.ParseFromString(read_message(client).payload) &&
                missing_response.has_register_unit(), "unit without target registers");
    cabinflow::protocol::v1::ControlRequest missing_setup;
    missing_setup.mutable_setup()->set_unit_id("missing.target");
    send_message(client, make_control("setup-missing", "session-text", "", missing_setup));
    require(missing_response.ParseFromString(read_message(client).payload) &&
                missing_response.has_setup(), "unit without target creates work");
    send_message(client, make_data("unknown-owned-target", missing_response.setup().work().work_id(),
        "missing.target", "cockpit.text.unknown", 0, fixture.clock().now_monotonic_ns(),
        1'000, text_payload("ignored")));
    read_delivery_error(client, "unknown-owned-target",
                        cabinflow::protocol::v1::DELIVERY_ERROR_UNKNOWN_TARGET);

    const auto expired_created = fixture.clock().now_monotonic_ns() - 2'000'000U;
    send_message(client, make_data("expired-before-enqueue", work_id,
                                         "dialogue.primary", "cockpit.text.expired", 0,
                                         expired_created, 1, text_payload("expired")));
    read_delivery_error(client, "expired-before-enqueue",
                        cabinflow::protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED);

    send_message(client, make_data("expired-unknown-target", work_id,
                                         "missing.target", "cockpit.text.expired", 0,
                                         expired_created, 1, text_payload("expired")));
    read_delivery_error(client, "expired-unknown-target",
                        cabinflow::protocol::v1::DELIVERY_ERROR_DEADLINE_EXCEEDED);

    send_message(client, make_data("text-partial-1", work_id,
                                         "dialogue.primary", "cockpit.text.input", 4,
                                         fixture.clock().now_monotonic_ns(), 1'000,
                                         text_payload("开")));
    send_message(client, make_data("text-partial-2", work_id,
                                         "dialogue.primary", "cockpit.text.input", 5,
                                         fixture.clock().now_monotonic_ns(), 1'000,
                                         text_payload("空")));
    wait_for_dialogue_event(fixture.logger(), 3);
    require(dialogue_message_ids(fixture.logger()) ==
                std::vector<std::string>{"text-0", "text-partial-1",
                                         "text-partial-2"},
            "partial TextInput reaches the target in sequence order");
    expect_no_response(client);

    auto final_client = connect_client(fixture.port());
    auto final_input = make_data(
        "text-final", work_id, "dialogue.primary", "cockpit.text.input", 6,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("调"), true);
    final_input.envelope.seat = cabinflow::protocol::SeatPosition::kDriver;
    send_message(final_client, final_input);
    wait_for_dialogue_event(fixture.logger(), 4);
    const auto output_message = read_message(final_client);
    cabinflow::agent::v1::TextOutput output;
    require(output.ParseFromString(output_message.payload) &&
                output.request_message_id() == "text-final" &&
                output.intent() == cabinflow::agent::v1::COCKPIT_INTENT_CLIMATE_ON &&
                output.text().find("未执行车控") != std::string::npos,
            "final text recognizes climate intent without executing vehicle control");
    require(output_message.envelope.topic == "cockpit.text.output" &&
                output_message.envelope.message_id != "text-final" &&
                output_message.envelope.trace_id == "trace-data" &&
                output_message.envelope.session_id == "session-text" &&
                output_message.envelope.work_id == work_id &&
                output_message.envelope.source_node == "dialogue.primary" &&
                output_message.envelope.target_node == "grpc-client" &&
                output_message.envelope.seat ==
                    cabinflow::protocol::SeatPosition::kDriver &&
                output_message.envelope.sequence == 0 &&
                output_message.envelope.is_final,
            "output has independent identity and follows the input work");
    expect_no_response(client);

    send_message(client, make_data("after-final", work_id, "dialogue.primary",
                                         "cockpit.text.input", 7,
                                         fixture.clock().now_monotonic_ns(), 1'000,
                                         text_payload("must reject")));
    read_delivery_error(client, "after-final",
                        cabinflow::protocol::v1::DELIVERY_ERROR_STREAM_FINALIZED);
}

void test_rule_based_intents() {
    const auto run_case = [](std::string_view text,
                             cabinflow::agent::v1::CockpitIntent expected) {
        GatewayFixture fixture;
        auto client = connect_client(fixture.port());
        const auto work_id = register_and_setup(client);
        send_message(client, make_data(
            "intent-final", work_id, "dialogue.primary", "cockpit.text.input", 0,
            fixture.clock().now_monotonic_ns(), 1'000,
            text_payload(std::string(text)), true));
        const auto message = read_message(client);
        cabinflow::agent::v1::TextOutput output;
        require(message.envelope.topic == "cockpit.text.output" &&
                    output.ParseFromString(message.payload) &&
                    output.request_message_id() == "intent-final" &&
                    output.intent() == expected &&
                    (expected == cabinflow::agent::v1::COCKPIT_INTENT_MUSIC ?
                        (output.has_music_command() && output.music_command().action() == cabinflow::agent::v1::MusicCommand::PLAY &&
                         output.music_command().keyword().empty() && output.music_command().result_index() == 0 &&
                         output.text().find("尚未执行") != std::string::npos) :
                        (!output.has_music_command() && output.text().find("未执行车控") != std::string::npos)),
                "rule-based output identifies only the stated cockpit intent");
        expect_no_response(client);
    };

    run_case("关闭空调", cabinflow::agent::v1::COCKPIT_INTENT_CLIMATE_OFF);
    run_case("打开座椅加热",
             cabinflow::agent::v1::COCKPIT_INTENT_SEAT_HEATING_ON);
    run_case("关闭座椅加热",
             cabinflow::agent::v1::COCKPIT_INTENT_SEAT_HEATING_OFF);
    run_case("打开天窗", cabinflow::agent::v1::COCKPIT_INTENT_UNRECOGNIZED);
    run_case("播放音乐", cabinflow::agent::v1::COCKPIT_INTENT_MUSIC);
}

void test_exit_rejects_later_data_without_cancelling_other_work() {
    GatewayFixture fixture;
    auto client = connect_client(fixture.port());
    const auto work_id = register_and_setup(client);

    cabinflow::protocol::v1::ControlRequest exit;
    exit.mutable_exit()->set_reason("user requested stop");
    send_message(client, make_control("exit-wrong-session", "other-session",
                                            work_id, exit));
    cabinflow::protocol::v1::ControlResponse rejected_exit;
    require(rejected_exit.ParseFromString(read_message(client).payload) &&
                rejected_exit.has_error(),
            "another session cannot exit this work");

    send_message(client, make_data(
        "before-exit", work_id, "dialogue.primary", "cockpit.text.input", 0,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("打开")));
    wait_for_dialogue_event(fixture.logger(), 1);

    send_message(client, make_control("exit-work", "session-text", work_id,
                                            exit));
    const auto exit_message = read_message(client);
    cabinflow::protocol::v1::ControlResponse exited;
    require(exited.ParseFromString(exit_message.payload) && exited.has_exit() &&
                exited.exit().work().work_id() == work_id &&
                exited.exit().work().state() ==
                    cabinflow::protocol::v1::WORK_STATE_EXITED,
            "successful Exit transitions the registered work");

    auto another_client = connect_client(fixture.port());
    send_message(another_client, make_data(
        "after-exit", work_id, "dialogue.primary", "cockpit.text.input", 1,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("空调")));
    read_delivery_error(another_client, "after-exit",
                        cabinflow::protocol::v1::DELIVERY_ERROR_WORK_CANCELLED);
    require(dialogue_message_ids(fixture.logger()) ==
                std::vector<std::string>{"before-exit"},
            "exited work does not deliver later data to the target");

    cabinflow::protocol::v1::ControlRequest setup;
    setup.mutable_setup()->set_unit_id("dialogue.primary");
    send_message(client, make_control("setup-after-exit", "session-text", "",
                                            setup));
    cabinflow::protocol::v1::ControlResponse new_setup;
    require(new_setup.ParseFromString(read_message(client).payload) &&
                new_setup.has_setup() &&
                new_setup.setup().work().work_id() != work_id,
            "Exit releases unit capacity for a new work");
    send_message(client, make_data(
        "new-work", new_setup.setup().work().work_id(), "dialogue.primary",
        "cockpit.text.input", 0, fixture.clock().now_monotonic_ns(), 1'000,
        text_payload("关闭")));
    wait_for_dialogue_event(fixture.logger(), 2);
}

void test_work_identity_is_checked_before_ledger_admission() {
    GatewayFixture fixture;
    auto client = connect_client(fixture.port());
    const auto work_id = register_and_setup(client);

    send_message(client, make_data(
        "missing-work", "work-not-created", "dialogue.primary",
        "cockpit.text.input", 0, fixture.clock().now_monotonic_ns(), 1'000,
        text_payload("打开")));
    read_delivery_error(client, "missing-work",
                        cabinflow::protocol::v1::DELIVERY_ERROR_WORK_NOT_FOUND);

    auto foreign_work = make_data(
        "foreign-work", work_id, "dialogue.primary", "cockpit.text.input", 0,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("打开"));
    foreign_work.envelope.session_id = "another-session";
    send_message(client, foreign_work);
    read_delivery_error(client, "foreign-work",
                        cabinflow::protocol::v1::DELIVERY_ERROR_WORK_NOT_FOUND);

    send_message(client, make_data(
        "valid-work", work_id, "dialogue.primary", "cockpit.text.input", 0,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("打开")));
    wait_for_dialogue_event(fixture.logger(), 1);
    require(dialogue_message_ids(fixture.logger()) ==
                std::vector<std::string>{"valid-work"},
            "rejected work identities do not enter the target or poison Ledger");
}

void test_external_owner_rejection_preserves_admission_and_internal_routing() {
    GatewayFixture fixture;
    auto client = connect_client(fixture.port());
    const auto work_id = register_and_setup(client);
    auto input = make_data("owner-retry", work_id, "other.primary", "cockpit.text.input", 0,
        fixture.clock().now_monotonic_ns(), 1'000, text_payload("打开空调"), true);
    send_message(client, input);
    const auto error = read_delivery_error(client, "owner-retry",
        cabinflow::protocol::v1::DELIVERY_ERROR_INVALID_ENVELOPE);
    require(error.message() == "target_node does not match work unit_id", "explicit ownership detail");
    require(fixture.other_calls() == 0 && dialogue_message_ids(fixture.logger()).empty(),
            "neither existing target receives rejected input");
    input.envelope.target_node = "dialogue.primary";
    send_message(client, input);
    cabinflow::agent::v1::TextOutput answer;
    require(answer.ParseFromString(read_message(client).payload) &&
                answer.request_message_id() == "owner-retry", "same final/id/sequence retry succeeds after target correction");
    auto internal = make_data("internal-stage", work_id, "other.primary", "pipeline.stage.probe", 0,
        fixture.clock().now_monotonic_ns(), 1'000, "opaque", true);
    auto reservation = fixture.runtime().reserve_target("other.primary");
    require(static_cast<bool>(reservation), "internal stage reserves second target");
    std::promise<void> completed;
    auto finished = completed.get_future();
    const auto admission = fixture.runtime().admit_reserved(std::move(reservation.reservation),
        internal, [](auto) {}, [&] { completed.set_value(); });
    require(admission.admitted && finished.wait_for(1s) == std::future_status::ready &&
                fixture.other_calls() == 1, "same work still crosses targets internally");
}

}  // namespace

int main() {
    try {
        test_typed_data_plane_over_grpc();
        test_rule_based_intents();
        test_exit_rejects_later_data_without_cancelling_other_work();
        test_work_identity_is_checked_before_ledger_admission();
        test_external_owner_rejection_preserves_admission_and_internal_routing();
        std::cout << "data plane gateway grpc test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "data plane gateway grpc test failed: " << error.what() << '\n';
        return 1;
    }
}
