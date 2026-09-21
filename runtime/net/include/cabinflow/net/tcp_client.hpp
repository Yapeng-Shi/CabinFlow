#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

#include <cabinflow/net/tcp_connection.hpp>

namespace cabinflow::net {

class EventLoop;

using ConnectErrorCallback = std::function<void(std::error_code)>;

class TcpClient final {
public:
    // 构造、配置、connect/disconnect/stop 与 connection 必须在 EventLoop owner 线程调用。
    TcpClient(EventLoop& loop, std::string name, std::string server_ip,
              std::uint16_t server_port);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void set_connection_callback(ConnectionCallback callback);
    void set_message_callback(MessageCallback callback);
    void set_connect_error_callback(ConnectErrorCallback callback);
    void connect();
    void disconnect();
    void stop();

    [[nodiscard]] TcpConnectionPtr connection() const;

private:
    struct Impl;

    void handle_new_connection(int connected_file_descriptor);
    void handle_connect_error(std::error_code error);
    void handle_connection_close(const TcpConnectionPtr& connection);

    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::net
