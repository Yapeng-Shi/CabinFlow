#include "channel.hpp"

#include <sys/epoll.h>

#include <exception>
#include <stdexcept>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

namespace cabinflow::net::detail {

Channel::Channel(EventLoop& loop, int file_descriptor)
    : loop_(loop), file_descriptor_(file_descriptor) {}

Channel::~Channel() {
    if (!is_none_event()) {
        std::terminate();
    }
}

int Channel::file_descriptor() const noexcept { return file_descriptor_; }

int Channel::events() const noexcept { return events_; }

bool Channel::is_none_event() const noexcept { return events_ == 0; }

int Channel::index() const noexcept { return index_; }

void Channel::set_index(int index) noexcept { index_ = index; }

void Channel::set_returned_events(int events) noexcept { returned_events_ = events; }

void Channel::set_read_callback(Callback callback) {
    read_callback_ = std::move(callback);
}

void Channel::set_write_callback(Callback callback) {
    write_callback_ = std::move(callback);
}

void Channel::set_close_callback(Callback callback) {
    close_callback_ = std::move(callback);
}

void Channel::set_error_callback(Callback callback) {
    error_callback_ = std::move(callback);
}

void Channel::tie(const std::shared_ptr<void>& owner) {
    owner_ = owner;
    tied_ = true;
}

void Channel::enable_reading() {
    events_ |= EPOLLIN;
    update();
}

void Channel::disable_reading() {
    events_ &= ~EPOLLIN;
    update();
}

void Channel::enable_writing() {
    events_ |= EPOLLOUT;
    update();
}

void Channel::disable_writing() {
    events_ &= ~EPOLLOUT;
    update();
}

void Channel::disable_all() {
    events_ = 0;
    update();
}

void Channel::remove() { loop_.remove_channel(*this); }

void Channel::handle_events() {
    if (tied_) {
        if (const std::shared_ptr<void> owner = owner_.lock()) {
            handle_events_with_guard();
        }
        return;
    }
    handle_events_with_guard();
}

void Channel::handle_events_with_guard() {
    if ((returned_events_ & EPOLLHUP) != 0 &&
        (returned_events_ & EPOLLIN) == 0 && close_callback_) {
        close_callback_();
    }
    if ((returned_events_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0 &&
        read_callback_) {
        read_callback_();
    }
    if ((returned_events_ & EPOLLOUT) != 0 && write_callback_) {
        write_callback_();
    }
    if ((returned_events_ & EPOLLERR) != 0 && error_callback_) {
        error_callback_();
    }
}

void Channel::update() { loop_.update_channel(*this); }

}  // namespace cabinflow::net::detail
