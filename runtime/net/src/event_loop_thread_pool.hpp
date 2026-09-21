#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {

class EventLoopThread;

class EventLoopThreadPool final {
public:
    EventLoopThreadPool(EventLoop& base_loop, std::size_t worker_count);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void start();
    void stop();
    [[nodiscard]] EventLoop& next_loop();
    [[nodiscard]] bool started() const noexcept;

private:
    void assert_in_base_loop_thread() const;

    EventLoop& base_loop_;
    std::size_t worker_count_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
    std::size_t next_{0};
    bool started_{false};
};

}  // namespace cabinflow::net::detail
