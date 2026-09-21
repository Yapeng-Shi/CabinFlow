#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "channel.hpp"
#include "inet_address.hpp"
#include "socket.hpp"

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {

class Acceptor final {
public:
    using NewConnectionCallback = std::function<void(Socket&&, const InetAddress&)>;

    Acceptor(EventLoop& loop, const InetAddress& listen_address, bool reuse_port);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void set_new_connection_callback(NewConnectionCallback callback);
    void listen();
    void stop();

    [[nodiscard]] bool listening() const noexcept;
    [[nodiscard]] std::uint16_t bound_port() const;

private:
    void assert_in_loop_thread() const;
    void handle_read();
    void handle_file_descriptor_exhaustion();

    EventLoop& loop_;
    Socket listen_socket_;
    Channel listen_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listening_{false};
    int idle_file_descriptor_{-1};
};

}  // namespace cabinflow::net::detail
