#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cabinflow::net::detail {
class Channel;
class Poller;
}

namespace cabinflow::net {

// 一个 EventLoop 只属于创建它的线程；外部线程只能通过 queue_in_loop 交接任务。
class EventLoop final {
public:
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // 在创建线程中阻塞处理 epoll 事件，直到 quit() 被请求。
    void loop();
    void quit();

    // 当前为 owner 线程时同步执行；否则转入 owner 线程的待执行队列。
    void run_in_loop(Task task);
    void queue_in_loop(Task task);

    [[nodiscard]] bool is_in_loop_thread() const noexcept;
    [[nodiscard]] std::size_t pending_task_count() const;

private:
    void assert_in_loop_thread() const;
    void wakeup();
    void handle_wakeup();
    void run_pending_tasks();
    void update_channel(detail::Channel& channel);
    void remove_channel(detail::Channel& channel);

    const std::thread::id owner_thread_id_;
    bool looping_{false};
    bool running_pending_tasks_{false};
    std::atomic<bool> quit_requested_{false};
    int wakeup_fd_{-1};
    std::unique_ptr<detail::Poller> poller_;
    std::unique_ptr<detail::Channel> wakeup_channel_;
    std::vector<detail::Channel*> active_channels_;
    mutable std::mutex task_mutex_;
    std::vector<Task> pending_tasks_;

    friend class detail::Channel;
};

}  // namespace cabinflow::net
