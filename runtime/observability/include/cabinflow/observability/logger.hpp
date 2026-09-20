#pragma once

#include <cabinflow/observability/event.hpp>

namespace cabinflow::observability {

class Logger {
public:
    virtual ~Logger() = default;

    virtual void log(const Event& event) = 0;
};

class ConsoleLogger final : public Logger {
public:
    void log(const Event& event) override;
};

}  // namespace cabinflow::observability
