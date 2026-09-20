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

    return 0;
}
