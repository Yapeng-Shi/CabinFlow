#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::gateway
