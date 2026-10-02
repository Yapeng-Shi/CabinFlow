#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <cabinflow/protocol/message_envelope.hpp>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::runtime {
class Clock;
class Runtime;
class UnitRegistry;
}

namespace cabinflow::gateway {

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
    void start();
    void stop();

    [[nodiscard]] std::uint16_t bound_port() const;

    // 仅按已准入的最终输入消息身份回传；payload 对 Gateway 保持不透明。
    // 调用方须先停止持有此 Gateway 回调的 Runtime worker，再销毁 Gateway。
    [[nodiscard]] bool send_data_output(
        const protocol::MessageEnvelope& request, std::string_view topic,
        std::string payload);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::gateway
