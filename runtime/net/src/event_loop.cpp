#include <cabinflow/net/event_loop.hpp>

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "channel.hpp"
#include "poller.hpp"

namespace cabinflow::net {
namespace {

constexpr int kPollTimeoutMs = 10'000;

int create_wakeup_fd() {
    const int file_descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (file_descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    return file_descriptor;
}

}  // namespace

EventLoop::EventLoop()
    : owner_thread_id_(std::this_thread::get_id()),
      wakeup_fd_(create_wakeup_fd()),
      poller_(std::make_unique<detail::Poller>(*this)),
      wakeup_channel_(std::make_unique<detail::Channel>(*this, wakeup_fd_)) {
    wakeup_channel_->set_read_callback([this] { handle_wakeup(); });
    wakeup_channel_->enable_reading();
}

EventLoop::~EventLoop() {
    assert_in_loop_thread();
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
    static_cast<void>(::close(wakeup_fd_));
}

void EventLoop::loop() {
    assert_in_loop_thread();
    if (looping_) {
        throw std::logic_error("EventLoop::loop called while already running");
    }

    looping_ = true;
    quit_requested_.store(false);
    while (!quit_requested_.load()) {
        active_channels_.clear();
        poller_->poll(kPollTimeoutMs, &active_channels_);
        for (detail::Channel* channel : active_channels_) {
            channel->handle_events();
        }
        run_pending_tasks();
    }
    looping_ = false;
}

void EventLoop::quit() {
    quit_requested_.store(true);
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

void EventLoop::run_in_loop(Task task) {
    if (!task) {
        throw std::invalid_argument("EventLoop task must not be empty");
    }
    if (is_in_loop_thread()) {
        task();
        return;
    }
    queue_in_loop(std::move(task));
}

void EventLoop::queue_in_loop(Task task) {
    if (!task) {
        throw std::invalid_argument("EventLoop task must not be empty");
    }
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    if (!is_in_loop_thread() || running_pending_tasks_) {
        wakeup();
    }
}

bool EventLoop::is_in_loop_thread() const noexcept {
    return owner_thread_id_ == std::this_thread::get_id();
}

std::size_t EventLoop::pending_task_count() const {
    std::lock_guard<std::mutex> lock(task_mutex_);
    return pending_tasks_.size();
}

void EventLoop::assert_in_loop_thread() const {
    if (!is_in_loop_thread()) {
        throw std::logic_error("EventLoop accessed outside owner thread");
    }
}

void EventLoop::wakeup() {
    const std::uint64_t one = 1;
    const auto written = ::write(wakeup_fd_, &one, sizeof(one));
    if (written != static_cast<ssize_t>(sizeof(one)) && errno != EAGAIN) {
        throw std::system_error(errno, std::generic_category(), "eventfd write");
    }
}

void EventLoop::handle_wakeup() {
    std::uint64_t value = 0;
    const auto read_count = ::read(wakeup_fd_, &value, sizeof(value));
    if (read_count != static_cast<ssize_t>(sizeof(value)) && errno != EAGAIN) {
        throw std::system_error(errno, std::generic_category(), "eventfd read");
    }
}

void EventLoop::run_pending_tasks() {
    std::vector<Task> tasks;
    running_pending_tasks_ = true;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks.swap(pending_tasks_);
    }
    for (const Task& task : tasks) {
        task();
    }
    running_pending_tasks_ = false;
}

void EventLoop::update_channel(detail::Channel& channel) {
    assert_in_loop_thread();
    poller_->update_channel(channel);
}

void EventLoop::remove_channel(detail::Channel& channel) {
    assert_in_loop_thread();
    poller_->remove_channel(channel);
}

}  // namespace cabinflow::net
