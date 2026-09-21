#include <cabinflow/net/tcp_client.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

#include "connector.hpp"
#include "inet_address.hpp"
#include "socket.hpp"

namespace cabinflow::net {

struct TcpClient::Impl {
    Impl(EventLoop& loop_arg, std::string name_arg, std::string server_ip,
         std::uint16_t server_port)
        : loop(loop_arg),
          name(std::move(name_arg)),
          connector(std::make_shared<detail::Connector>(
              loop, detail::InetAddress(server_ip, server_port))) {}

    EventLoop& loop;
    std::string name;
    std::shared_ptr<detail::Connector> connector;
    ConnectionCallback connection_callback;
    MessageCallback message_callback;
    ConnectErrorCallback connect_error_callback;
    TcpConnectionPtr connection;
    std::size_t next_connection_id{1};
    bool connect_requested{false};
};

TcpClient::TcpClient(EventLoop& loop, std::string name, std::string server_ip,
                     std::uint16_t server_port) {
    if (!loop.is_in_loop_thread()) {
        throw std::logic_error("TcpClient must be created in EventLoop owner thread");
    }
    impl_ = std::make_unique<Impl>(loop, std::move(name), std::move(server_ip), server_port);
    impl_->connector->set_new_connection_callback([this](detail::Socket&& socket) {
        handle_new_connection(socket.release());
    });
    impl_->connector->set_connect_error_callback(
        [this](std::error_code error) { handle_connect_error(error); });
}

TcpClient::~TcpClient() {
    if (!impl_->loop.is_in_loop_thread()) {
        std::terminate();
    }
    stop();
}

void TcpClient::set_connection_callback(ConnectionCallback callback) {
    if (!impl_->loop.is_in_loop_thread() || impl_->connect_requested) {
        throw std::logic_error("TcpClient callbacks must be set before connect");
    }
    impl_->connection_callback = std::move(callback);
}

void TcpClient::set_message_callback(MessageCallback callback) {
    if (!impl_->loop.is_in_loop_thread() || impl_->connect_requested) {
        throw std::logic_error("TcpClient callbacks must be set before connect");
    }
    impl_->message_callback = std::move(callback);
}

void TcpClient::set_connect_error_callback(ConnectErrorCallback callback) {
    if (!impl_->loop.is_in_loop_thread() || impl_->connect_requested) {
        throw std::logic_error("TcpClient callbacks must be set before connect");
    }
    impl_->connect_error_callback = std::move(callback);
}

void TcpClient::connect() {
    if (!impl_->loop.is_in_loop_thread() || impl_->connect_requested || impl_->connection) {
        throw std::logic_error("TcpClient has an active connection attempt");
    }
    impl_->connect_requested = true;
    impl_->connector->start();
}

void TcpClient::disconnect() {
    if (!impl_->loop.is_in_loop_thread()) {
        throw std::logic_error("TcpClient accessed outside EventLoop owner thread");
    }
    impl_->connect_requested = false;
    impl_->connector->stop();
    if (impl_->connection) {
        impl_->connection->shutdown();
    }
}

void TcpClient::stop() {
    if (!impl_->loop.is_in_loop_thread()) {
        throw std::logic_error("TcpClient accessed outside EventLoop owner thread");
    }
    impl_->connect_requested = false;
    impl_->connector->stop();
    if (impl_->connection) {
        impl_->connection->force_close();
    }
}

TcpConnectionPtr TcpClient::connection() const {
    if (!impl_->loop.is_in_loop_thread()) {
        throw std::logic_error("TcpClient accessed outside EventLoop owner thread");
    }
    return impl_->connection;
}

void TcpClient::handle_new_connection(int connected_file_descriptor) {
    if (!impl_->loop.is_in_loop_thread() || !impl_->connect_requested || impl_->connection) {
        detail::Socket socket(connected_file_descriptor);
        return;
    }

    const std::string name = impl_->name + "-" + std::to_string(impl_->next_connection_id++);
    const TcpConnectionPtr connection =
        TcpConnection::create(impl_->loop, name, connected_file_descriptor);
    connection->set_connection_callback(impl_->connection_callback);
    connection->set_message_callback(impl_->message_callback);
    connection->set_close_callback(
        [this](const TcpConnectionPtr& value) { handle_connection_close(value); });
    impl_->connection = connection;
    connection->establish();
}

void TcpClient::handle_connect_error(std::error_code error) {
    if (!impl_->loop.is_in_loop_thread() || !impl_->connect_requested) {
        return;
    }
    impl_->connect_requested = false;
    if (impl_->connect_error_callback) {
        impl_->connect_error_callback(error);
    }
}

void TcpClient::handle_connection_close(const TcpConnectionPtr& connection) {
    if (impl_->connection == connection) {
        impl_->connection.reset();
        impl_->connect_requested = false;
        impl_->connector->stop();
    }
}

}  // namespace cabinflow::net
