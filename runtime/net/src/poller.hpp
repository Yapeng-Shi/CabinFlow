#pragma once

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {
class Channel;

class Poller final {
public:
    explicit Poller(EventLoop& loop);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void poll(int timeout_ms, std::vector<Channel*>* active_channels);
    void update_channel(Channel& channel);
    void remove_channel(Channel& channel);

private:
    void assert_in_loop_thread() const;
    void update(int operation, Channel& channel);

    EventLoop& loop_;
    int epoll_fd_{-1};
    std::vector<epoll_event> events_;
    std::unordered_map<int, Channel*> channels_;
};

}  // namespace cabinflow::net::detail
