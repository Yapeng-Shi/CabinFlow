#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <cabinflow/net/tcp_connection.hpp>

namespace cabinflow::net {

class EventLoop;

class TcpServer final {
public:
    // 构造、配置、start/stop 与 connection_count 必须在 EventLoop owner 线程调用。
    TcpServer(EventLoop& loop, std::string name, std::string listen_ip,
              std::uint16_t listen_port);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void set_connection_callback(ConnectionCallback callback);
    void set_message_callback(MessageCallback callback);
    void set_worker_count(std::size_t worker_count);
    void start();
    void stop();

    [[nodiscard]] std::uint16_t bound_port() const;
    [[nodiscard]] std::size_t connection_count() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::net
