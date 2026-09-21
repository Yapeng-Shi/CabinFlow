#include <cabinflow/gateway/control_response_builder.hpp>

#include <utility>

#include "message_id.hpp"

namespace cabinflow::gateway {
namespace {

[[nodiscard]] bool has_matching_work_id(const protocol::v1::WorkInfo& work,
                                         std::string_view expected_work_id) {
    return !expected_work_id.empty() && work.work_id() == expected_work_id;
}

[[nodiscard]] bool resolve_error_work_id(
    const ValidatedControlRequest& request, std::string* work_id) {
    switch (request.command.command_case()) {
        case protocol::v1::ControlRequest::kSetup:
        case protocol::v1::ControlRequest::kRegisterUnit:
        case protocol::v1::ControlRequest::COMMAND_NOT_SET:
            work_id->clear();
            return true;
        case protocol::v1::ControlRequest::kPause:
        case protocol::v1::ControlRequest::kExit:
            *work_id = request.message.envelope.work_id;
            return true;
        case protocol::v1::ControlRequest::kTaskInfo:
            if (request.command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_WORK) {
                *work_id = request.message.envelope.work_id;
                return true;
            }
            if (request.command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_SESSION) {
                work_id->clear();
                return true;
            }
            // scope 未指定时不从 work_id 猜测类型；错误响应使用空 work 身份。
            work_id->clear();
            return true;
    }
    return false;
}

[[nodiscard]] bool resolve_response_work_id(
    const ValidatedControlRequest& request,
    const protocol::v1::ControlResponse& response, std::string* work_id) {
    switch (response.result_case()) {
        case protocol::v1::ControlResponse::kSetup:
            *work_id = response.setup().work().work_id();
            return has_matching_work_id(response.setup().work(), *work_id);
        case protocol::v1::ControlResponse::kPause:
            *work_id = request.message.envelope.work_id;
            return has_matching_work_id(response.pause().work(), *work_id);
        case protocol::v1::ControlResponse::kExit:
            *work_id = request.message.envelope.work_id;
            return has_matching_work_id(response.exit().work(), *work_id);
        case protocol::v1::ControlResponse::kTaskInfo:
            if (request.command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_WORK) {
                *work_id = request.message.envelope.work_id;
                return !work_id->empty();
            }
            if (request.command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_SESSION) {
                work_id->clear();
                return true;
            }
            return false;
        case protocol::v1::ControlResponse::kRegisterUnit:
            work_id->clear();
            return true;
        case protocol::v1::ControlResponse::kError:
            return resolve_error_work_id(request, work_id);
        case protocol::v1::ControlResponse::RESULT_NOT_SET:
            return false;
    }
    return false;
}

[[nodiscard]] bool response_matches_command(
    const ValidatedControlRequest& request,
    const protocol::v1::ControlResponse& response) {
    if (response.result_case() == protocol::v1::ControlResponse::kError) {
        return true;
    }

    switch (request.command.command_case()) {
        case protocol::v1::ControlRequest::kSetup:
            return response.result_case() == protocol::v1::ControlResponse::kSetup;
        case protocol::v1::ControlRequest::kPause:
            return response.result_case() == protocol::v1::ControlResponse::kPause;
        case protocol::v1::ControlRequest::kExit:
            return response.result_case() == protocol::v1::ControlResponse::kExit;
        case protocol::v1::ControlRequest::kTaskInfo:
            return response.result_case() == protocol::v1::ControlResponse::kTaskInfo;
        case protocol::v1::ControlRequest::kRegisterUnit:
            return response.result_case() ==
                   protocol::v1::ControlResponse::kRegisterUnit;
        case protocol::v1::ControlRequest::COMMAND_NOT_SET:
            return false;
    }
    return false;
}

}  // namespace

ControlResponseBuilder::ControlResponseBuilder(
    const runtime::Clock& clock, ResponseSequencer& sequencer,
    std::uint32_t response_ttl_ms)
    : clock_(clock), sequencer_(sequencer), response_ttl_ms_(response_ttl_ms) {}

ControlResponseBuildResult ControlResponseBuilder::build(
    const ValidatedControlRequest& request,
    const ControlServiceResult& service_result,
    std::string_view connection_id) const {
    if (response_ttl_ms_ == 0U) {
        return {{}, ControlResponseBuildError::kInvalidResponseTtl};
    }

    auto response = service_result.response;
    std::string response_work_id;
    if (!response_matches_command(request, response) ||
        !resolve_response_work_id(request, response, &response_work_id)) {
        return {{}, ControlResponseBuildError::kInvalidServiceResponse};
    }

    const auto sequence = sequencer_.next(request.message.envelope.session_id,
                                          response_work_id, "control.response",
                                          connection_id);
    if (!sequence) {
        return {{}, ControlResponseBuildError::kSequenceFailure};
    }

    // 响应与请求是两条消息，使用独立的不可预测身份而非复用 request ID。
    const auto message_id = generate_gateway_message_id("control-response-");
    if (message_id.empty()) {
        return {{}, ControlResponseBuildError::kMessageIdGenerationFailure};
    }

    response.set_request_message_id(request.message.envelope.message_id);
    protocol::Message message;
    auto& envelope = message.envelope;
    envelope.schema_version = protocol::kCurrentSchemaVersion;
    envelope.message_id = message_id;
    envelope.trace_id = request.message.envelope.trace_id;
    envelope.session_id = request.message.envelope.session_id;
    envelope.work_id = std::move(response_work_id);
    envelope.source_node = request.message.envelope.target_node;
    envelope.target_node = request.message.envelope.source_node;
    envelope.topic = "control.response";
    envelope.kind = response.result_case() == protocol::v1::ControlResponse::kError
                        ? protocol::MessageKind::kError
                        : protocol::MessageKind::kData;
    envelope.sequence = sequence.sequence;
    envelope.created_monotonic_ns = clock_.now_monotonic_ns();
    envelope.ttl_ms = response_ttl_ms_;
    envelope.is_final = true;

    if (!response.SerializeToString(&message.payload)) {
        return {{}, ControlResponseBuildError::kSerializationFailure};
    }
    return {std::move(message), ControlResponseBuildError::kNone};
}

}  // namespace cabinflow::gateway
