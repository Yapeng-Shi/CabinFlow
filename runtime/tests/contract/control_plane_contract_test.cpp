#include <cstdint>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cabinflow/gateway/control_envelope_validator.hpp>
#include <cabinflow/gateway/control_response_builder.hpp>
#include <cabinflow/gateway/control_service.hpp>
#include <cabinflow/gateway/response_sequencer.hpp>

#include "fake_clock.hpp"

namespace {

using cabinflow::gateway::ControlEnvelopeValidator;
using cabinflow::gateway::ControlResponseBuilder;
using cabinflow::gateway::ControlResponseBuildError;
using cabinflow::gateway::ControlErrorCode;
using cabinflow::gateway::ControlService;
using cabinflow::gateway::ControlValidationError;
using cabinflow::gateway::ResponseSequencer;
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
using cabinflow::protocol::v1::ControlRequest;

constexpr std::uint64_t kControlNowNs = 1'000'000'000ULL;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

Message make_request(std::string message_id, std::string session_id,
                     std::string work_id, const ControlRequest& request) {
    Message message;
    message.envelope.message_id = std::move(message_id);
    message.envelope.trace_id = "trace-1";
    message.envelope.session_id = std::move(session_id);
    message.envelope.work_id = std::move(work_id);
    message.envelope.source_node = "gateway-client";
    message.envelope.target_node = "control-service";
    message.envelope.topic = "control.request";
    message.envelope.kind = MessageKind::kData;
    message.envelope.created_monotonic_ns = kControlNowNs;
    message.envelope.ttl_ms = 1000;
    require(request.SerializeToString(&message.payload), "serialize control request");
    return message;
}

ControlRequest register_unit_request(std::string unit_id,
                                     std::uint32_t max_concurrent_work) {
    ControlRequest request;
    auto* command = request.mutable_register_unit();
    command->set_unit_id(std::move(unit_id));
    command->add_capabilities("speech");
    command->set_max_concurrent_work(max_concurrent_work);
    return request;
}

ControlRequest setup_request(std::string unit_id) {
    ControlRequest request;
    request.mutable_setup()->set_unit_id(std::move(unit_id));
    return request;
}

void test_control_envelope_rules() {
    cabinflow::test::FakeClock clock(kControlNowNs);
    ControlEnvelopeValidator validator(clock);

    const auto setup = setup_request("asr.primary");
    const auto valid_setup = validator.validate(
        make_request("setup-1", "session-1", "", setup));
    require(static_cast<bool>(valid_setup), "setup accepts empty work id");

    const auto invalid_setup = validator.validate(
        make_request("setup-2", "session-1", "client-work", setup));
    require(invalid_setup.error == ControlValidationError::kInvalidControlIdentity,
            "setup rejects client work id");

    const auto registration = register_unit_request("asr.primary", 1);
    const auto valid_registration = validator.validate(
        make_request("register-1", "", "", registration));
    require(static_cast<bool>(valid_registration), "registration permits empty session id");

    const auto invalid_registration = validator.validate(
        make_request("register-2", "session-1", "", registration));
    require(invalid_registration.error ==
                ControlValidationError::kInvalidControlIdentity,
            "registration rejects session id");

    auto unsupported_schema = make_request("setup-3", "session-1", "", setup);
    unsupported_schema.envelope.schema_version = 2;
    const auto schema_result = validator.validate(unsupported_schema);
    require(schema_result.error == ControlValidationError::kUnsupportedSchemaVersion &&
                schema_result.can_return_error && schema_result.should_close_connection,
            "unsupported schema requests error then connection close");

    auto malformed_payload = make_request("setup-4", "session-1", "", setup);
    malformed_payload.payload = "not a control protobuf";
    const auto malformed_result = validator.validate(malformed_payload);
    require(malformed_result.error == ControlValidationError::kMalformedControlPayload &&
                !malformed_result.can_return_error &&
                malformed_result.should_close_connection,
            "malformed typed payload closes without a fallback parser");

    auto before_deadline = make_request("setup-before-deadline", "session-1", "", setup);
    before_deadline.envelope.created_monotonic_ns = 1;
    require(static_cast<bool>(validator.validate(before_deadline)),
            "control request remains valid just before its deadline");

    auto expired = make_request("setup-expired", "session-1", "", setup);
    expired.envelope.created_monotonic_ns = 0;
    const auto expired_result = validator.validate(expired);
    require(expired_result.error == ControlValidationError::kExpired &&
                expired_result.can_return_error &&
                !expired_result.should_close_connection,
            "control request at its deadline returns a correlated error");

    auto future = make_request("setup-future", "session-1", "", setup);
    future.envelope.created_monotonic_ns = kControlNowNs + 1;
    const auto future_result = validator.validate(future);
    require(future_result.error == ControlValidationError::kInvalidEnvelope &&
                future_result.can_return_error &&
                !future_result.should_close_connection,
            "future monotonic timestamp is an invalid control envelope");

    auto zero_ttl = make_request("setup-zero-ttl", "session-1", "", setup);
    zero_ttl.envelope.ttl_ms = 0;
    require(validator.validate(zero_ttl).error ==
                ControlValidationError::kInvalidEnvelope,
            "control request requires a nonzero ttl");

    ControlRequest work_query;
    work_query.mutable_task_info()->set_scope(
        cabinflow::protocol::v1::TASK_INFO_SCOPE_WORK);
    const auto invalid_work_query = validator.validate(
        make_request("info-1", "session-1", "", work_query));
    require(invalid_work_query.error ==
                ControlValidationError::kInvalidControlIdentity,
            "work task query requires envelope work id");
}

void test_control_service_lifecycle_and_setup_idempotency() {
    cabinflow::runtime::UnitRegistry registry;
    cabinflow::test::FakeClock clock(kControlNowNs);
    ControlEnvelopeValidator validator(clock);
    ControlService service(registry);

    const auto invalid_registration = validator.validate(
        make_request("register-0", "", "", register_unit_request("asr.primary", 0)));
    const auto invalid_registration_response = service.handle(*invalid_registration.request);
    require(invalid_registration_response.response.has_error() &&
                invalid_registration_response.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kInvalidRequest),
            "registration requires positive capacity");

    const auto registration = validator.validate(
        make_request("register-1", "", "", register_unit_request("asr.primary", 1)));
    require(static_cast<bool>(registration), "registration validates");
    const auto registered = service.handle(*registration.request);
    require(registered.response.has_register_unit() &&
                registered.response.register_unit().unit_id() == "asr.primary",
            "unit registration returns ready unit");

    const auto first_setup = validator.validate(
        make_request("setup-1", "session-1", "", setup_request("asr.primary")));
    require(static_cast<bool>(first_setup), "first setup validates");
    const auto first_response = service.handle(*first_setup.request);
    require(first_response.response.has_setup() &&
                !first_response.response.setup().work().work_id().empty(),
            "setup creates a server work id");
    const auto work_id = first_response.response.setup().work().work_id();

    const auto full_unit_setup = validator.validate(
        make_request("setup-2", "session-1", "", setup_request("asr.primary")));
    const auto full_unit_response = service.handle(*full_unit_setup.request);
    require(full_unit_response.response.has_error() &&
                full_unit_response.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kUnitUnavailable),
            "new setup must not exceed registered unit capacity");

    const auto repeated_setup = validator.validate(
        make_request("setup-1", "session-1", "", setup_request("asr.primary")));
    const auto repeated_response = service.handle(*repeated_setup.request);
    require(repeated_response.response.has_setup() &&
                repeated_response.response.setup().work().work_id() == work_id,
            "same setup identity returns original work id");

    cabinflow::test::FakeClock late_clock(kControlNowNs + 1'000'000'000ULL);
    ControlEnvelopeValidator late_validator(late_clock);
    const auto expired_replay = late_validator.validate(
        make_request("setup-1", "session-1", "", setup_request("asr.primary")));
    require(expired_replay.error == ControlValidationError::kExpired &&
                registry.find_session_work("session-1").size() == 1,
            "expired replay is rejected before setup idempotency returns the work");

    const auto conflicting_setup = validator.validate(
        make_request("setup-1", "session-1", "", setup_request("tts.primary")));
    const auto conflicting_response = service.handle(*conflicting_setup.request);
    require(conflicting_response.response.has_error() &&
                conflicting_response.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kConflict),
            "same setup identity with different content conflicts");

    ControlRequest empty_pause;
    empty_pause.mutable_pause()->set_reason("");
    const auto invalid_pause = validator.validate(
        make_request("pause-1", "session-1", work_id, empty_pause));
    const auto invalid_pause_response = service.handle(*invalid_pause.request);
    require(invalid_pause_response.response.has_error() &&
                invalid_pause_response.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kInvalidRequest),
            "pause requires explicit reason");

    ControlRequest pause;
    pause.mutable_pause()->set_reason("driver interrupted");
    const auto paused = service.handle(*validator.validate(
        make_request("pause-2", "session-1", work_id, pause)).request);
    require(paused.response.has_pause() &&
                paused.response.pause().work().state() ==
                    cabinflow::protocol::v1::WORK_STATE_PAUSED,
            "pause changes work state");

    ControlRequest task_info;
    task_info.mutable_task_info()->set_scope(
        cabinflow::protocol::v1::TASK_INFO_SCOPE_SESSION);
    const auto queried = service.handle(*validator.validate(
        make_request("info-1", "session-1", "", task_info)).request);
    require(queried.response.has_task_info() && queried.response.task_info().works_size() == 1,
            "session query uses explicit scope and returns work");

    ControlRequest exit;
    exit.mutable_exit()->set_reason("session complete");
    const auto exited = service.handle(*validator.validate(
        make_request("exit-1", "session-1", work_id, exit)).request);
    require(exited.response.has_exit() &&
                exited.response.exit().work().state() ==
                    cabinflow::protocol::v1::WORK_STATE_EXITED,
            "exit releases work through registry");

    const auto pause_after_exit = service.handle(*validator.validate(
        make_request("pause-3", "session-1", work_id, pause)).request);
    require(pause_after_exit.response.has_error() &&
                pause_after_exit.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kInvalidWorkState),
            "exit rejects later work commands");

    ControlRequest unknown_command;
    const auto unknown = service.handle(*validator.validate(
        make_request("unknown-1", "session-1", "", unknown_command)).request);
    require(unknown.response.has_error() &&
                unknown.response.error().code() ==
                    static_cast<std::uint32_t>(ControlErrorCode::kInvalidRequest),
            "unknown control command returns structured error");
}

void test_response_identity_and_sequence_are_independent() {
    cabinflow::test::FakeClock validation_clock(kControlNowNs);
    ControlEnvelopeValidator validator(validation_clock);
    const auto setup = validator.validate(
        make_request("setup-request", "session-1", "", setup_request("asr.primary")));
    require(static_cast<bool>(setup), "setup response input validates");

    cabinflow::gateway::ControlServiceResult setup_result;
    auto* setup_work = setup_result.response.mutable_setup()->mutable_work();
    setup_work->set_work_id("work-created");
    setup_work->set_unit_id("asr.primary");
    setup_work->set_state(cabinflow::protocol::v1::WORK_STATE_RUNNING);

    cabinflow::test::FakeClock clock(1234);
    ResponseSequencer sequencer;
    ControlResponseBuilder builder(clock, sequencer, 600);
    const auto first = builder.build(*setup.request, setup_result, "connection-1");
    require(static_cast<bool>(first), "build setup response");
    require(first.message.envelope.message_id != "setup-request" &&
                !first.message.envelope.message_id.empty(),
            "response receives a distinct message id");
    require(first.message.envelope.trace_id == "trace-1" &&
                first.message.envelope.session_id == "session-1" &&
                first.message.envelope.work_id == "work-created",
            "response preserves trace/session and adopts setup work id");
    require(first.message.envelope.source_node == "control-service" &&
                first.message.envelope.target_node == "gateway-client" &&
                first.message.envelope.topic == "control.response" &&
                first.message.envelope.sequence == 0 &&
                first.message.envelope.created_monotonic_ns == 1234 &&
                first.message.envelope.ttl_ms == 600 && first.message.envelope.is_final &&
                first.message.envelope.kind == MessageKind::kData,
            "response envelope follows the control response contract");

    cabinflow::protocol::v1::ControlResponse decoded;
    require(decoded.ParseFromString(first.message.payload) &&
                decoded.request_message_id() == "setup-request" && decoded.has_setup(),
            "response payload carries explicit request correlation");

    const auto repeated = builder.build(*setup.request, setup_result, "connection-1");
    require(static_cast<bool>(repeated) &&
                repeated.message.envelope.message_id != first.message.envelope.message_id &&
                repeated.message.envelope.sequence == 1,
            "replayed result gets a new message id and response sequence");

    ControlRequest pause;
    pause.mutable_pause()->set_reason("driver interrupted");
    const auto pause_request = validator.validate(
        make_request("pause-request", "session-1", "work-created", pause));
    require(static_cast<bool>(pause_request), "pause response input validates");
    cabinflow::gateway::ControlServiceResult pause_error;
    pause_error.response.mutable_error()->set_code(1);
    pause_error.response.mutable_error()->set_message("work is unavailable");
    const auto error_response = builder.build(*pause_request.request, pause_error,
                                              "connection-1");
    require(static_cast<bool>(error_response) &&
                error_response.message.envelope.work_id == "work-created" &&
                error_response.message.envelope.kind == MessageKind::kError,
            "work error retains request work identity and uses ERROR kind");

    ControlRequest registration = register_unit_request("tts.primary", 1);
    const auto register_request = validator.validate(
        make_request("register-request", "", "", registration));
    require(static_cast<bool>(register_request), "registration response input validates");
    cabinflow::gateway::ControlServiceResult register_result;
    register_result.response.mutable_register_unit()->set_unit_id("tts.primary");
    register_result.response.mutable_register_unit()->set_state(
        cabinflow::protocol::v1::UNIT_STATE_READY);
    const auto first_register = builder.build(*register_request.request, register_result,
                                              "connection-1");
    const auto second_register = builder.build(*register_request.request, register_result,
                                               "connection-2");
    require(static_cast<bool>(first_register) && static_cast<bool>(second_register) &&
                first_register.message.envelope.sequence == 0 &&
                second_register.message.envelope.sequence == 0,
            "registration response streams are isolated by internal connection id");

    ControlRequest unspecified_task_info;
    unspecified_task_info.mutable_task_info();
    const auto unspecified_request = validator.validate(
        make_request("task-info-unspecified", "session-1", "", unspecified_task_info));
    require(static_cast<bool>(unspecified_request),
            "unspecified task scope reaches service for structured rejection");
    cabinflow::gateway::ControlServiceResult unspecified_error;
    unspecified_error.response.mutable_error()->set_code(1);
    unspecified_error.response.mutable_error()->set_message("task info scope is required");
    const auto unspecified_response = builder.build(*unspecified_request.request,
                                                    unspecified_error, "connection-1");
    require(static_cast<bool>(unspecified_response) &&
                unspecified_response.message.envelope.kind == MessageKind::kError &&
                unspecified_response.message.envelope.work_id.empty(),
            "unspecified task scope returns a structured error without work guessing");

    auto incomplete_setup = setup_result;
    incomplete_setup.response.mutable_setup()->mutable_work()->clear_work_id();
    require(builder.build(*setup.request, incomplete_setup, "connection-1").error ==
                ControlResponseBuildError::kInvalidServiceResponse,
            "gateway rejects a setup response without a work identity");
}

void test_concurrent_setup_replay_and_response_sequences() {
    cabinflow::runtime::UnitRegistry registry;
    cabinflow::test::FakeClock clock(kControlNowNs);
    ControlEnvelopeValidator validator(clock);
    ControlService service(registry);

    const auto registration = validator.validate(
        make_request("register-concurrent", "", "",
                     register_unit_request("dialogue.primary", 1)));
    require(static_cast<bool>(registration), "concurrent unit registration validates");
    require(service.handle(*registration.request).response.has_register_unit(),
            "concurrent unit registration succeeds");

    const auto setup = validator.validate(
        make_request("setup-concurrent", "session-concurrent", "",
                     setup_request("dialogue.primary")));
    require(static_cast<bool>(setup), "concurrent setup validates");

    std::mutex results_mutex;
    std::vector<std::string> work_ids;
    std::vector<std::thread> workers;
    for (int index = 0; index != 8; ++index) {
        workers.emplace_back([&] {
            const auto response = service.handle(*setup.request);
            require(response.response.has_setup(), "concurrent setup returns setup");
            std::lock_guard<std::mutex> lock(results_mutex);
            work_ids.push_back(response.response.setup().work().work_id());
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    require(work_ids.size() == 8 && !work_ids.front().empty(),
            "concurrent setup produced all responses");
    for (const auto& work_id : work_ids) {
        require(work_id == work_ids.front(),
                "concurrent setup replay created more than one work");
    }
    require(registry.find_session_work("session-concurrent").size() == 1,
            "concurrent setup left more than one registry work");

    ResponseSequencer sequencer;
    std::vector<std::uint64_t> sequences;
    std::mutex sequences_mutex;
    workers.clear();
    for (int index = 0; index != 16; ++index) {
        workers.emplace_back([&] {
            const auto sequence = sequencer.next(
                "session-concurrent", "work-1", "control.response", "");
            require(static_cast<bool>(sequence), "concurrent sequence allocation succeeds");
            std::lock_guard<std::mutex> lock(sequences_mutex);
            sequences.push_back(sequence.sequence);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    std::sort(sequences.begin(), sequences.end());
    require(sequences.size() == 16, "concurrent sequence allocation returned all values");
    for (std::size_t index = 0; index != sequences.size(); ++index) {
        require(sequences[index] == index,
                "concurrent response sequence was duplicated or skipped");
    }
}

}  // namespace

int main() {
    try {
        test_control_envelope_rules();
        test_control_service_lifecycle_and_setup_idempotency();
        test_response_identity_and_sequence_are_independent();
        test_concurrent_setup_replay_and_response_sequences();
        std::cout << "control plane contract test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "control plane contract test failed: " << error.what() << '\n';
        return 1;
    }
}
