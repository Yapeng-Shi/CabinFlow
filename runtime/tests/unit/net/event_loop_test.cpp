#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <cabinflow/net/event_loop.hpp>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_cross_thread_queue_preserves_owner_thread() {
    std::promise<cabinflow::net::EventLoop*> loop_ready;
    auto loop_result = loop_ready.get_future();
    std::thread loop_thread([&loop_ready] {
        cabinflow::net::EventLoop loop;
        loop_ready.set_value(&loop);
        loop.loop();
    });

    auto* loop = loop_result.get();
    auto task_done = std::make_shared<std::promise<bool>>();
    auto task_result = task_done->get_future();
    loop->queue_in_loop([loop, task_done] {
        task_done->set_value(loop->is_in_loop_thread());
    });

    require(task_result.wait_for(2s) == std::future_status::ready,
            "cross-thread queue did not wake EventLoop");
    require(task_result.get(), "queued task did not run in EventLoop owner thread");

    // 调用方在 quit 后 join，保证传出的非拥有指针不再被访问。
    loop->quit();
    loop_thread.join();
}

}  // namespace

int main() {
    try {
        test_cross_thread_queue_preserves_owner_thread();
        std::cout << "event loop test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "event loop test failed: " << error.what() << '\n';
        return 1;
    }
}
