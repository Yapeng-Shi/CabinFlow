#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <cabinflow/transport/transport.hpp>

namespace cabinflow::transport::detail {
class ZmqTransportState;
}

namespace cabinflow::transport {

struct ZmqTransportConfig {
    // The local publisher binds here. Leave empty for a receive-only transport.
    std::string bind_endpoint;
    // The local subscriber connects here. Leave empty for a send-only transport.
    std::string connect_endpoint;
};

// ZeroMQ socket 只能由同一个线程调用。这里由 I/O 线程独占 socket，业务
// handler 则在独立回调线程执行，因此回调可安全调用 publish/unsubscribe。
class ZmqTransport final : public ITransport {
public:
    explicit ZmqTransport(ZmqTransportConfig config);
    ~ZmqTransport() override;

    ZmqTransport(const ZmqTransport&) = delete;
    ZmqTransport& operator=(const ZmqTransport&) = delete;
    ZmqTransport(ZmqTransport&&) = delete;
    ZmqTransport& operator=(ZmqTransport&&) = delete;

    [[nodiscard]] TransportError publish(
        const protocol::Message& message) override;
    [[nodiscard]] SubscribeResult subscribe(std::string_view topic,
                                             MessageHandler handler) override;

    // When bind_endpoint uses a wildcard TCP port, this is the concrete
    // endpoint returned by ZeroMQ after bind succeeds.
    [[nodiscard]] std::string_view bound_endpoint() const noexcept;

private:
    std::shared_ptr<detail::ZmqTransportState> state_;
    std::string bound_endpoint_;
};

}  // namespace cabinflow::transport
