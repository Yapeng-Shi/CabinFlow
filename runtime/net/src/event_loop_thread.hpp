#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {

class EventLoopThread final {
public:
    using ThreadInitCallback = std::function<void(EventLoop&)>;

    explicit EventLoopThread(ThreadInitCallback init_callback = {});
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    EventLoop& start_loop();
    void stop();

private:
    void run();

    ThreadInitCallback init_callback_;
    std::mutex mutex_;
    std::condition_variable changed_;
    EventLoop* loop_{nullptr};
    std::exception_ptr startup_error_;
    bool started_{false};
    std::thread thread_;
};

}  // namespace cabinflow::net::detail
