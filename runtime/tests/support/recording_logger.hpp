#pragma once

#include <mutex>
#include <vector>

#include <cabinflow/observability/logger.hpp>

namespace cabinflow::test {

class RecordingLogger final : public observability::Logger {
public:
    void log(const observability::Event& event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }

    [[nodiscard]] std::vector<observability::Event> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<observability::Event> events_;
};

}  // namespace cabinflow::test
