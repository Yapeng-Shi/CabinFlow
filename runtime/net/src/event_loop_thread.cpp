#include "event_loop_thread.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

namespace cabinflow::net::detail {

EventLoopThread::EventLoopThread(ThreadInitCallback init_callback)
    : init_callback_(std::move(init_callback)) {}

EventLoopThread::~EventLoopThread() { stop(); }

EventLoop& EventLoopThread::start_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (started_) {
        throw std::logic_error("EventLoopThread already started");
    }
    started_ = true;
    thread_ = std::thread([this] { run(); });
    changed_.wait(lock, [this] { return loop_ != nullptr || startup_error_ != nullptr; });
    if (startup_error_) {
        lock.unlock();
        thread_.join();
        std::rethrow_exception(startup_error_);
    }
    return *loop_;
}

void EventLoopThread::stop() {
    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop = loop_;
    }
    if (loop != nullptr) {
        loop->quit();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void EventLoopThread::run() {
    try {
        EventLoop loop;
        if (init_callback_) {
            init_callback_(loop);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = &loop;
        }
        changed_.notify_all();
        loop.loop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = nullptr;
        }
        changed_.notify_all();
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startup_error_ = std::current_exception();
        }
        changed_.notify_all();
    }
}

}  // namespace cabinflow::net::detail
