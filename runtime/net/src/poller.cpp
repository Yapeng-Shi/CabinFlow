#include "poller.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#include "channel.hpp"

#include <cabinflow/net/event_loop.hpp>

namespace cabinflow::net::detail {
namespace {

constexpr int kNewChannel = -1;
constexpr int kAddedChannel = 1;
constexpr int kDeletedChannel = 2;
constexpr std::size_t kInitialEventCount = 16;

}  // namespace

Poller::Poller(EventLoop& loop) : loop_(loop), events_(kInitialEventCount) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "epoll_create1");
    }
}

Poller::~Poller() {
    if (epoll_fd_ >= 0) {
        static_cast<void>(::close(epoll_fd_));
    }
}

void Poller::poll(int timeout_ms, std::vector<Channel*>* active_channels) {
    const int event_count = ::epoll_wait(epoll_fd_, events_.data(),
                                         static_cast<int>(events_.size()),
                                         timeout_ms);
    if (event_count < 0) {
        if (errno == EINTR) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }

    for (int index = 0; index < event_count; ++index) {
        auto* channel = static_cast<Channel*>(events_[index].data.ptr);
        channel->set_returned_events(events_[index].events);
        active_channels->push_back(channel);
    }
    if (static_cast<std::size_t>(event_count) == events_.size()) {
        events_.resize(events_.size() * 2);
    }
}

void Poller::update_channel(Channel& channel) {
    assert_in_loop_thread();
    const int index = channel.index();
    if (index == kNewChannel || index == kDeletedChannel) {
        channels_[channel.file_descriptor()] = &channel;
        channel.set_index(kAddedChannel);
        update(EPOLL_CTL_ADD, channel);
        return;
    }

    if (channel.is_none_event()) {
        update(EPOLL_CTL_DEL, channel);
        channel.set_index(kDeletedChannel);
        return;
    }
    update(EPOLL_CTL_MOD, channel);
}

void Poller::remove_channel(Channel& channel) {
    assert_in_loop_thread();
    const auto iterator = channels_.find(channel.file_descriptor());
    if (iterator == channels_.end() || iterator->second != &channel ||
        !channel.is_none_event()) {
        throw std::logic_error("invalid Channel removal");
    }

    channels_.erase(iterator);
    if (channel.index() == kAddedChannel) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel.set_index(kNewChannel);
}

void Poller::assert_in_loop_thread() const {
    if (!loop_.is_in_loop_thread()) {
        throw std::logic_error("Poller accessed outside EventLoop owner thread");
    }
}

void Poller::update(int operation, Channel& channel) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(channel.events());
    event.data.ptr = &channel;
    if (::epoll_ctl(epoll_fd_, operation, channel.file_descriptor(), &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

}  // namespace cabinflow::net::detail
