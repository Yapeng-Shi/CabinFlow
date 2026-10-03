#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cabinflow/protocol/message_envelope.hpp>
#include <cabinflow/runtime/runtime.hpp>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::runtime {
class Clock;
class Runtime;
class UnitRegistry;
}

namespace cabinflow::gateway {

struct DataTaskOutput {
    std::string topic;
    std::string payload;
    protocol::MessageKind kind{protocol::MessageKind::kData};
};

// 应用只管理当前同步任务的占位/撤销/完成；Gateway 不解析业务 Protobuf。
// 所有回调不得抛异常，必须在 start 前完整装配；引用对象须活到 Runtime drain 完成。
struct DataTaskHooks {
    std::function<bool(const protocol::Message&)> try_reserve;
    std::function<void(const protocol::Message&)> rollback;
    std::function<DataTaskOutput(
        const protocol::Message&,
        std::optional<runtime::Runtime::TargetDeliveryFailure>)> complete;
    std::function<void(const protocol::MessageEnvelope&)> cancel;
};

// TCP control-plane entrypoint. 调用方必须先配置，再在 EventLoop owner 线程启动和停止。
class ControlGateway final {
public:
    ControlGateway(net::EventLoop& loop, std::string name, std::string listen_ip,
                   std::uint16_t listen_port, runtime::UnitRegistry& registry,
                   runtime::Runtime& runtime,
                   const runtime::Clock& clock, std::uint32_t response_ttl_ms);
    ~ControlGateway();

    ControlGateway(const ControlGateway&) = delete;
    ControlGateway& operator=(const ControlGateway&) = delete;
    ControlGateway(ControlGateway&&) = delete;
    ControlGateway& operator=(ControlGateway&&) = delete;

    void set_worker_count(std::size_t worker_count);
    void set_data_task_hooks(DataTaskHooks hooks);
    void start();
    void stop();

    [[nodiscard]] std::uint16_t bound_port() const;

    // 仅按已准入的最终输入消息身份回传；payload 对 Gateway 保持不透明。
    // 调用方须先停止持有此 Gateway 回调的 Runtime worker，再销毁 Gateway。
    [[nodiscard]] bool send_data_output(
        const protocol::MessageEnvelope& request, std::string_view topic,
        std::string payload, protocol::MessageKind kind);

    // 使用相同响应身份长度及数值字段最大编码开销，在提交终态前检查完整帧体。
    [[nodiscard]] static bool data_output_fits(
        const protocol::MessageEnvelope& request, const DataTaskOutput& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::gateway
