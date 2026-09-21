#include <cabinflow/gateway/control_service.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cabinflow::gateway {
namespace {

struct SetupKey {
    std::string session_id;
    std::string message_id;

    [[nodiscard]] bool operator==(const SetupKey& other) const noexcept {
        return session_id == other.session_id && message_id == other.message_id;
    }
};

struct SetupKeyHash {
    [[nodiscard]] std::size_t operator()(const SetupKey& key) const {
        return std::hash<std::string>{}(key.session_id) ^
               (std::hash<std::string>{}(key.message_id) << 1U);
    }
};

struct SetupRecord {
    std::string serialized_command;
    runtime::WorkInfo work;
};

[[nodiscard]] protocol::v1::WorkState to_proto_work_state(
    runtime::WorkState state) {
    switch (state) {
        case runtime::WorkState::kRunning:
            return protocol::v1::WORK_STATE_RUNNING;
        case runtime::WorkState::kPaused:
            return protocol::v1::WORK_STATE_PAUSED;
        case runtime::WorkState::kExited:
            return protocol::v1::WORK_STATE_EXITED;
    }
    return protocol::v1::WORK_STATE_UNSPECIFIED;
}

[[nodiscard]] protocol::v1::UnitState to_proto_unit_state(
    runtime::UnitState state) {
    switch (state) {
        case runtime::UnitState::kReady:
            return protocol::v1::UNIT_STATE_READY;
        case runtime::UnitState::kBusy:
            return protocol::v1::UNIT_STATE_BUSY;
        case runtime::UnitState::kStopping:
            return protocol::v1::UNIT_STATE_STOPPING;
    }
    return protocol::v1::UNIT_STATE_UNSPECIFIED;
}

void set_work_info(protocol::v1::WorkInfo* target,
                   const runtime::WorkInfo& source) {
    target->set_work_id(source.work_id);
    target->set_unit_id(source.unit_id);
    target->set_state(to_proto_work_state(source.state));
}

[[nodiscard]] ControlServiceResult make_error(ControlErrorCode code,
                                               std::string message) {
    ControlServiceResult result;
    auto* error = result.response.mutable_error();
    error->set_code(static_cast<std::uint32_t>(code));
    error->set_message(std::move(message));
    return result;
}

[[nodiscard]] ControlServiceResult from_registry_error(
    runtime::UnitRegistryError error) {
    switch (error) {
        case runtime::UnitRegistryError::kInvalidUnit:
            return make_error(ControlErrorCode::kInvalidRequest,
                              "invalid unit registration or setup request");
        case runtime::UnitRegistryError::kUnitAlreadyRegistered:
            return make_error(ControlErrorCode::kConflict,
                              "unit is already registered");
        case runtime::UnitRegistryError::kUnitNotFound:
            return make_error(ControlErrorCode::kUnitNotFound,
                              "requested unit is not registered");
        case runtime::UnitRegistryError::kUnitUnavailable:
            return make_error(ControlErrorCode::kUnitUnavailable,
                              "requested unit has no available capacity");
        case runtime::UnitRegistryError::kWorkNotFound:
            return make_error(ControlErrorCode::kWorkNotFound,
                              "work does not exist in this session");
        case runtime::UnitRegistryError::kWorkNotRunning:
            return make_error(ControlErrorCode::kInvalidWorkState,
                              "work state does not allow this command");
        case runtime::UnitRegistryError::kWorkIdGenerationFailure:
            return make_error(ControlErrorCode::kInternal,
                              "failed to create a unique work id");
        case runtime::UnitRegistryError::kNone:
            break;
    }
    return make_error(ControlErrorCode::kInternal, "unknown registry failure");
}

}  // namespace

struct ControlService::State {
    std::mutex mutex;
    std::unordered_map<SetupKey, SetupRecord, SetupKeyHash> completed_setups;
};

ControlService::ControlService(runtime::UnitRegistry& registry)
    : registry_(registry), state_(std::make_unique<State>()) {}

ControlService::~ControlService() = default;

ControlServiceResult ControlService::handle(
    const ValidatedControlRequest& request) {
    const auto& envelope = request.message.envelope;
    const auto& command = request.command;

    switch (command.command_case()) {
        case protocol::v1::ControlRequest::kRegisterUnit: {
            const auto& registration = command.register_unit();
            runtime::UnitRegistration unit;
            unit.unit_id = registration.unit_id();
            unit.max_concurrent_work = registration.max_concurrent_work();
            unit.capabilities.assign(registration.capabilities().begin(),
                                     registration.capabilities().end());
            const auto result = registry_.register_unit(std::move(unit));
            if (!result) {
                return from_registry_error(result.error);
            }

            ControlServiceResult response;
            auto* registered = response.response.mutable_register_unit();
            registered->set_unit_id(registration.unit_id());
            registered->set_state(to_proto_unit_state(result.state));
            return response;
        }
        case protocol::v1::ControlRequest::kSetup: {
            std::string serialized_command;
            if (!command.SerializeToString(&serialized_command)) {
                return make_error(ControlErrorCode::kInternal,
                                  "failed to serialize setup request");
            }

            // 查重和创建被同一把锁覆盖，重放 setup 不会并发占用第二个 work 槽位。
            std::lock_guard<std::mutex> lock(state_->mutex);
            const SetupKey key{envelope.session_id, envelope.message_id};
            const auto previous = state_->completed_setups.find(key);
            if (previous != state_->completed_setups.end()) {
                if (previous->second.serialized_command != serialized_command) {
                    return make_error(ControlErrorCode::kConflict,
                                      "setup message id conflicts with prior content");
                }
                ControlServiceResult response;
                set_work_info(response.response.mutable_setup()->mutable_work(),
                              previous->second.work);
                return response;
            }

            const auto created = registry_.create_work(envelope.session_id,
                                                       command.setup().unit_id());
            if (!created) {
                return from_registry_error(created.error);
            }
            state_->completed_setups.emplace(
                key, SetupRecord{std::move(serialized_command), created.work});

            ControlServiceResult response;
            set_work_info(response.response.mutable_setup()->mutable_work(),
                          created.work);
            return response;
        }
        case protocol::v1::ControlRequest::kPause: {
            if (command.pause().reason().empty()) {
                return make_error(ControlErrorCode::kInvalidRequest,
                                  "pause reason is required");
            }
            const auto paused = registry_.pause_work(envelope.session_id,
                                                     envelope.work_id);
            if (!paused) {
                return from_registry_error(paused.error);
            }
            ControlServiceResult response;
            set_work_info(response.response.mutable_pause()->mutable_work(), paused.work);
            return response;
        }
        case protocol::v1::ControlRequest::kExit: {
            if (command.exit().reason().empty()) {
                return make_error(ControlErrorCode::kInvalidRequest,
                                  "exit reason is required");
            }
            const auto exited = registry_.exit_work(envelope.session_id,
                                                    envelope.work_id);
            if (!exited) {
                return from_registry_error(exited.error);
            }
            ControlServiceResult response;
            set_work_info(response.response.mutable_exit()->mutable_work(), exited.work);
            return response;
        }
        case protocol::v1::ControlRequest::kTaskInfo: {
            ControlServiceResult response;
            auto* task_info = response.response.mutable_task_info();
            if (command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_WORK) {
                const auto work = registry_.find_work(envelope.session_id,
                                                      envelope.work_id);
                if (!work) {
                    return from_registry_error(work.error);
                }
                set_work_info(task_info->add_works(), work.work);
                return response;
            }
            if (command.task_info().scope() ==
                protocol::v1::TASK_INFO_SCOPE_SESSION) {
                for (const auto& work : registry_.find_session_work(envelope.session_id)) {
                    set_work_info(task_info->add_works(), work);
                }
                return response;
            }
            return make_error(ControlErrorCode::kInvalidRequest,
                              "task info scope is required");
        }
        case protocol::v1::ControlRequest::COMMAND_NOT_SET:
            return make_error(ControlErrorCode::kInvalidRequest,
                              "control command is required");
    }

    return make_error(ControlErrorCode::kInvalidRequest,
                      "unknown control command");
}

}  // namespace cabinflow::gateway
