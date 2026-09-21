#include <iostream>
#include <string_view>

#include <cabinflow/runtime/bounded_queue.hpp>

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

}  // namespace

int main() {
    using cabinflow::runtime::BoundedQueue;
    using cabinflow::runtime::QueuePushResult;

    BoundedQueue<int> queue(2);
    if (!expect(queue.capacity() == 2, "configured capacity") ||
        !expect(queue.try_push(10) == QueuePushResult::kAccepted,
                "first enqueue") ||
        !expect(queue.try_push(20) == QueuePushResult::kAccepted,
                "second enqueue") ||
        !expect(queue.try_push(30) == QueuePushResult::kFull,
                "full queue is rejected") ||
        !expect(queue.size() == 2, "full queue retains accepted items")) {
        return 1;
    }

    const auto first = queue.try_pop();
    const auto second = queue.try_pop();
    const auto empty = queue.try_pop();
    if (!expect(first.has_value() && *first == 10, "fifo first item") ||
        !expect(second.has_value() && *second == 20, "fifo second item") ||
        !expect(!empty.has_value(), "empty queue") ||
        !expect(queue.size() == 0, "empty size")) {
        return 1;
    }

    BoundedQueue<int> reserved_queue(1);
    {
        auto reservation = reserved_queue.try_reserve();
        if (!expect(static_cast<bool>(reservation), "queue capacity can be reserved") ||
            !expect(reserved_queue.reserved_size() == 1,
                    "reservation is visible to backpressure") ||
            !expect(reserved_queue.try_push(99) == QueuePushResult::kFull,
                    "reserved capacity rejects competing enqueue") ||
            !expect(reservation.reservation.commit(42) ==
                        cabinflow::runtime::QueueCommitResult::kCommitted,
                    "reserved capacity commits without a second full check")) {
            return 1;
        }
    }
    const auto committed = reserved_queue.wait_pop();
    if (!expect(committed.has_value() && *committed == 42,
                "committed reservation reaches consumer") ||
        !expect(reserved_queue.reserved_size() == 0,
                "commit releases the reservation count")) {
        return 1;
    }

    BoundedQueue<int> stopped_queue(1);
    if (!expect(stopped_queue.try_push(7) == QueuePushResult::kAccepted,
                "queue accepts item before stop")) {
        return 1;
    }
    stopped_queue.close_and_discard();
    if (!expect(!stopped_queue.wait_pop().has_value(),
                "stopped queue discards work before handler execution") ||
        !expect(stopped_queue.try_push(8) == QueuePushResult::kClosed,
                "stopped queue no longer accepts messages")) {
        return 1;
    }

    return 0;
}
