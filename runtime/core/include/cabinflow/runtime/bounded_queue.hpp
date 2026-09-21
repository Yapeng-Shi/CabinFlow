#pragma once

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cabinflow::runtime {

enum class QueuePushResult {
    kAccepted,
    kFull,
    kClosed,
};

enum class QueueReserveError {
    kNone,
    kFull,
    kClosed,
};

enum class QueueCommitResult {
    kCommitted,
    kClosed,
};

// A thread-safe bounded queue. Reservations reserve capacity before a caller
// mutates external admission state, so a later commit cannot observe kFull.
template <typename T>
class BoundedQueue final {
public:
    class Reservation final {
    public:
        Reservation() = default;
        ~Reservation() { release(); }

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        Reservation(Reservation&& other) noexcept
            : queue_(std::exchange(other.queue_, nullptr)),
              active_(std::exchange(other.active_, false)) {}

        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                release();
                queue_ = std::exchange(other.queue_, nullptr);
                active_ = std::exchange(other.active_, false);
            }
            return *this;
        }

        [[nodiscard]] QueueCommitResult commit(T value) {
            if (queue_ == nullptr || !active_) {
                return QueueCommitResult::kClosed;
            }
            return queue_->commit_reservation(*this, std::move(value));
        }

        [[nodiscard]] bool active() const noexcept { return active_; }

    private:
        friend class BoundedQueue;

        explicit Reservation(BoundedQueue* queue) noexcept
            : queue_(queue), active_(true) {}

        void release() noexcept {
            if (queue_ != nullptr && active_) {
                queue_->release_reservation(*this);
            }
        }

        BoundedQueue* queue_{nullptr};
        bool active_{false};
    };

    struct ReservationResult {
        Reservation reservation;
        QueueReserveError error{QueueReserveError::kNone};

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == QueueReserveError::kNone;
        }
    };

    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    [[nodiscard]] QueuePushResult try_push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return QueuePushResult::kClosed;
        }
        if (items_.size() + reserved_ == capacity_) {
            return QueuePushResult::kFull;
        }

        items_.push_back(std::move(value));
        changed_.notify_one();
        return QueuePushResult::kAccepted;
    }

    [[nodiscard]] ReservationResult try_reserve() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return {Reservation{}, QueueReserveError::kClosed};
        }
        if (items_.size() + reserved_ == capacity_) {
            return {Reservation{}, QueueReserveError::kFull};
        }

        ++reserved_;
        return {Reservation(this), QueueReserveError::kNone};
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

    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) {
            return std::nullopt;
        }

        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

    void close_and_discard() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        items_.clear();
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::size_t reserved_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reserved_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] QueueCommitResult commit_reservation(Reservation& reservation,
                                                        T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reservation.active_) {
            return QueueCommitResult::kClosed;
        }

        reservation.active_ = false;
        reservation.queue_ = nullptr;
        --reserved_;
        if (closed_) {
            changed_.notify_all();
            return QueueCommitResult::kClosed;
        }

        items_.push_back(std::move(value));
        changed_.notify_one();
        return QueueCommitResult::kCommitted;
    }

    void release_reservation(Reservation& reservation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reservation.active_) {
            return;
        }

        reservation.active_ = false;
        reservation.queue_ = nullptr;
        --reserved_;
        changed_.notify_all();
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> items_;
    std::size_t reserved_{0};
    bool closed_{false};
};

}  // namespace cabinflow::runtime
