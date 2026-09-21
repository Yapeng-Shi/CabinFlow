#include "event_loop_thread_pool.hpp"

#include <stdexcept>

#include <cabinflow/net/event_loop.hpp>

#include "event_loop_thread.hpp"

namespace cabinflow::net::detail {

EventLoopThreadPool::EventLoopThreadPool(EventLoop& base_loop, std::size_t worker_count)
    : base_loop_(base_loop), worker_count_(worker_count) {}

EventLoopThreadPool::~EventLoopThreadPool() { stop(); }

void EventLoopThreadPool::start() {
    assert_in_base_loop_thread();
    if (started_) {
        throw std::logic_error("EventLoopThreadPool already started");
    }

    started_ = true;
    threads_.reserve(worker_count_);
    loops_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
        auto thread = std::make_unique<EventLoopThread>();
        loops_.push_back(&thread->start_loop());
        threads_.push_back(std::move(thread));
    }
}

void EventLoopThreadPool::stop() {
    if (!started_) {
        return;
    }
    assert_in_base_loop_thread();
    // 先请求所有 worker 退出，再按对象顺序 join；不让析构阶段留下后台 EventLoop。
    for (const std::unique_ptr<EventLoopThread>& thread : threads_) {
        thread->stop();
    }
    threads_.clear();
    loops_.clear();
    next_ = 0;
    started_ = false;
}

EventLoop& EventLoopThreadPool::next_loop() {
    assert_in_base_loop_thread();
    if (!started_) {
        throw std::logic_error("EventLoopThreadPool has not started");
    }
    if (loops_.empty()) {
        return base_loop_;
    }
    EventLoop& loop = *loops_[next_];
    next_ = (next_ + 1) % loops_.size();
    return loop;
}

bool EventLoopThreadPool::started() const noexcept { return started_; }

void EventLoopThreadPool::assert_in_base_loop_thread() const {
    if (!base_loop_.is_in_loop_thread()) {
        throw std::logic_error("EventLoopThreadPool accessed outside base EventLoop owner thread");
    }
}

}  // namespace cabinflow::net::detail
