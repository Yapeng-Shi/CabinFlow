#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cabinflow::runtime {

enum class QueuePushResult {
    kAccepted,
    kFull,
};

// A thread-safe, non-blocking queue. kFull is the explicit backpressure
// result; callers decide whether to reject, retry, or cancel a work item.
template <typename T>
class BoundedQueue final {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    [[nodiscard]] QueuePushResult try_push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.size() == capacity_) {
            return QueuePushResult::kFull;
        }

        items_.push_back(std::move(value));
        return QueuePushResult::kAccepted;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }

        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> items_;
};

}  // namespace cabinflow::runtime
