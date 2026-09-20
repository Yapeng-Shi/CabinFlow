#pragma once

namespace cabinflow::transport {

class Subscription {
public:
    virtual ~Subscription() = default;

    virtual void unsubscribe() noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
};

}  // namespace cabinflow::transport
