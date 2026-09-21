#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cabinflow::net {

class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
// bytes 仅在回调执行期间有效；上层协议解析后应立即复制或消费。
using MessageCallback = std::function<void(const TcpConnectionPtr&, std::string_view)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

enum class ConnectionState { connecting, connected, disconnecting, disconnected };

class TcpConnection final : public std::enable_shared_from_this<TcpConnection> {
public:
    // 调用方在 EventLoop owner 线程中创建并配置回调，再调用 establish() 开始收包。
    static TcpConnectionPtr create(EventLoop& loop, std::string name,
                                   int connected_file_descriptor);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void set_connection_callback(ConnectionCallback callback);
    void set_message_callback(MessageCallback callback);
    void set_close_callback(CloseCallback callback);

    void establish();
    void send(std::string bytes);
    void shutdown();
    void force_close();
    void start_reading();
    void stop_reading();
    void set_tcp_no_delay(bool enabled);

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const std::string& local_endpoint() const noexcept;
    [[nodiscard]] const std::string& peer_endpoint() const noexcept;
    [[nodiscard]] ConnectionState state() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool is_reading() const noexcept;

private:
    struct Impl;

    explicit TcpConnection(std::unique_ptr<Impl> implementation);
    void establish_in_loop();
    void send_in_loop(std::string bytes);
    void shutdown_in_loop();
    void force_close_in_loop();
    void start_reading_in_loop();
    void stop_reading_in_loop();
    void handle_read();
    void handle_write();
    void handle_close();

    std::unique_ptr<Impl> impl_;
};

}  // namespace cabinflow::net
