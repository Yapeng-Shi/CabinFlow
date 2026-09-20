#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include <cabinflow/runtime/bounded_queue.hpp>

namespace {

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kConsumerCount = 3;
constexpr std::size_t kMessagesPerProducer = 25'000;
constexpr std::size_t kMessageCount =
    kProducerCount * kMessagesPerProducer;

}  // namespace

int main() {
    cabinflow::runtime::BoundedQueue<std::uint64_t> queue(64);
    std::atomic<bool> start{false};
    std::atomic<std::size_t> producers_remaining{kProducerCount};
    std::atomic<std::size_t> consumed_count{0};
    std::atomic<std::uint64_t> produced_sum{0};
    std::atomic<std::uint64_t> consumed_sum{0};

    std::vector<std::thread> threads;
    threads.reserve(kProducerCount + kConsumerCount);

    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
        threads.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0; index < kMessagesPerProducer; ++index) {
                const auto value = static_cast<std::uint64_t>(
                    producer * kMessagesPerProducer + index + 1);
                while (queue.try_push(value) ==
                       cabinflow::runtime::QueuePushResult::kFull) {
                    std::this_thread::yield();
                }
                produced_sum.fetch_add(value, std::memory_order_relaxed);
            }
            producers_remaining.fetch_sub(1, std::memory_order_release);
        });
    }

    for (std::size_t consumer = 0; consumer < kConsumerCount; ++consumer) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (true) {
                const auto value = queue.try_pop();
                if (value.has_value()) {
                    consumed_sum.fetch_add(*value, std::memory_order_relaxed);
                    consumed_count.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (producers_remaining.load(std::memory_order_acquire) == 0 &&
                    queue.size() == 0) {
                    return;
                }
                std::this_thread::yield();
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    const auto expected_sum = static_cast<std::uint64_t>(kMessageCount) *
                              (static_cast<std::uint64_t>(kMessageCount) + 1) /
                              2;
    if (produced_sum.load(std::memory_order_relaxed) != expected_sum ||
        consumed_count.load(std::memory_order_relaxed) != kMessageCount ||
        consumed_sum.load(std::memory_order_relaxed) != expected_sum) {
        std::cerr << "bounded queue stress invariant failed\n";
        return 1;
    }

    return 0;
}
