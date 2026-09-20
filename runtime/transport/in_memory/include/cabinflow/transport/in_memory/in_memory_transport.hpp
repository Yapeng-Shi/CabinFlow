#pragma once

#include <memory>

#include <cabinflow/transport/transport.hpp>

namespace cabinflow::transport::detail {
struct InMemoryTransportState;
}

namespace cabinflow::transport {

// Deterministic local transport for tests and demo wiring. publish() invokes
// matching handlers synchronously on the calling thread.
class InMemoryTransport final : public ITransport {
public:
    InMemoryTransport();
    ~InMemoryTransport() override;

    InMemoryTransport(const InMemoryTransport&) = delete;
    InMemoryTransport& operator=(const InMemoryTransport&) = delete;
    InMemoryTransport(InMemoryTransport&&) = delete;
    InMemoryTransport& operator=(InMemoryTransport&&) = delete;

    [[nodiscard]] TransportError publish(
        const protocol::Message& message) override;
    [[nodiscard]] SubscribeResult subscribe(std::string_view topic,
                                             MessageHandler handler) override;

private:
    std::shared_ptr<detail::InMemoryTransportState> state_;
};

}  // namespace cabinflow::transport
