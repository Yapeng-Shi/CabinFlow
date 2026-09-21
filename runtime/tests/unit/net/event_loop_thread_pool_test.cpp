#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

#include <cabinflow/net/event_loop.hpp>

#include "event_loop_thread.hpp"
#include "event_loop_thread_pool.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_event_loop_thread_initializes_and_runs_tasks() {
    std::promise<bool> initialized;
    std::future<bool> initialized_result = initialized.get_future();
    cabinflow::net::detail::EventLoopThread thread(
        [&initialized](cabinflow::net::EventLoop& loop) {
            initialized.set_value(loop.is_in_loop_thread());
        });
    cabinflow::net::EventLoop& loop = thread.start_loop();
    require(initialized_result.wait_for(1s) == std::future_status::ready &&
                initialized_result.get(),
            "EventLoopThread did not run initialization in its owner thread");

    std::promise<bool> completed;
    std::future<bool> completed_result = completed.get_future();
    loop.queue_in_loop([&loop, &completed] { completed.set_value(loop.is_in_loop_thread()); });
    require(completed_result.wait_for(1s) == std::future_status::ready && completed_result.get(),
            "EventLoopThread did not run queued task in its owner thread");
    thread.stop();
}

void test_thread_pool_rotates_workers_and_uses_base_for_zero_workers() {
    cabinflow::net::EventLoop base_loop;
    cabinflow::net::detail::EventLoopThreadPool pool(base_loop, 2);
    pool.start();

    cabinflow::net::EventLoop& first = pool.next_loop();
    cabinflow::net::EventLoop& second = pool.next_loop();
    cabinflow::net::EventLoop& third = pool.next_loop();
    require(&first != &second && &first == &third,
            "EventLoopThreadPool did not distribute loops round-robin");

    std::promise<bool> first_completed;
    std::promise<bool> second_completed;
    std::future<bool> first_result = first_completed.get_future();
    std::future<bool> second_result = second_completed.get_future();
    first.queue_in_loop([&first, &first_completed] {
        first_completed.set_value(first.is_in_loop_thread());
    });
    second.queue_in_loop([&second, &second_completed] {
        second_completed.set_value(second.is_in_loop_thread());
    });
    require(first_result.wait_for(1s) == std::future_status::ready && first_result.get(),
            "first worker did not process queued task");
    require(second_result.wait_for(1s) == std::future_status::ready && second_result.get(),
            "second worker did not process queued task");
    pool.stop();

    cabinflow::net::detail::EventLoopThreadPool zero_worker_pool(base_loop, 0);
    zero_worker_pool.start();
    require(&zero_worker_pool.next_loop() == &base_loop,
            "zero-worker pool did not return base EventLoop");
    zero_worker_pool.stop();
}

}  // namespace

int main() {
    try {
        test_event_loop_thread_initializes_and_runs_tasks();
        test_thread_pool_rotates_workers_and_uses_base_for_zero_workers();
        std::cout << "event loop thread pool test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "event loop thread pool test failed: " << error.what() << '\n';
        return 1;
    }
}
