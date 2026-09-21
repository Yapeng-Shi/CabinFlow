#pragma once

#include <functional>
#include <memory>
#include <system_error>

#include "channel.hpp"
#include "inet_address.hpp"
#include "socket.hpp"

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {

class Connector final : public std::enable_shared_from_this<Connector> {
public:
    using NewConnectionCallback = std::function<void(Socket&&)>;
    using ConnectErrorCallback = std::function<void(std::error_code)>;

    Connector(EventLoop& loop, InetAddress server_address);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void set_new_connection_callback(NewConnectionCallback callback);
    void set_connect_error_callback(ConnectErrorCallback callback);
    void start();
    void stop();

private:
    enum class State { disconnected, connecting, connected };

    void assert_in_loop_thread() const;
    void connect();
    void handle_write();
    void handle_error();
    void complete_connect();
    void fail_connect(std::error_code error);
    void retire_channel();

    EventLoop& loop_;
    InetAddress server_address_;
    State state_{State::disconnected};
    bool connect_requested_{false};
    std::unique_ptr<Socket> socket_;
    std::shared_ptr<Channel> channel_;
    NewConnectionCallback new_connection_callback_;
    ConnectErrorCallback connect_error_callback_;
};

}  // namespace cabinflow::net::detail
