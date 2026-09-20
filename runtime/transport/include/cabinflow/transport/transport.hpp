#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <cabinflow/protocol/message.hpp>
#include <cabinflow/transport/subscription.hpp>
#include <cabinflow/transport/transport_error.hpp>

namespace cabinflow::transport {

using MessageHandler = std::function<void(const protocol::Message&)>;

struct SubscribeResult {
    std::unique_ptr<Subscription> subscription;
    TransportError error{TransportError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == TransportError::kNone && subscription != nullptr;
    }
};

class ITransport {
public:
    virtual ~ITransport() = default;

    [[nodiscard]] virtual TransportError publish(
        const protocol::Message& message) = 0;
    [[nodiscard]] virtual SubscribeResult subscribe(
        std::string_view topic, MessageHandler handler) = 0;
};

}  // namespace cabinflow::transport
