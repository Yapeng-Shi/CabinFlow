#include "connector.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

namespace cabinflow::net::detail {

Connector::Connector(EventLoop& loop, InetAddress server_address)
    : loop_(loop), server_address_(std::move(server_address)) {}

Connector::~Connector() {
    if (channel_) {
        std::terminate();
    }
}

void Connector::set_new_connection_callback(NewConnectionCallback callback) {
    assert_in_loop_thread();
    if (state_ != State::disconnected) {
        throw std::logic_error("Connector callbacks must be set before connect");
    }
    new_connection_callback_ = std::move(callback);
}

void Connector::set_connect_error_callback(ConnectErrorCallback callback) {
    assert_in_loop_thread();
    if (state_ != State::disconnected) {
        throw std::logic_error("Connector callbacks must be set before connect");
    }
    connect_error_callback_ = std::move(callback);
}

void Connector::start() {
    assert_in_loop_thread();
    if (state_ != State::disconnected) {
        throw std::logic_error("Connector is already active");
    }
    connect_requested_ = true;
    connect();
}

void Connector::stop() {
    assert_in_loop_thread();
    connect_requested_ = false;
    if (state_ == State::connecting) {
        retire_channel();
        socket_.reset();
        state_ = State::disconnected;
    }
    if (state_ == State::connected) {
        state_ = State::disconnected;
    }
}

void Connector::assert_in_loop_thread() const {
    if (!loop_.is_in_loop_thread()) {
        throw std::logic_error("Connector accessed outside EventLoop owner thread");
    }
}

void Connector::connect() {
    socket_ = std::make_unique<Socket>(Socket::create_nonblocking(server_address_.family()));
    const int result = ::connect(socket_->file_descriptor(), server_address_.sockaddr_ptr(),
                                 server_address_.sockaddr_length());
    if (result == 0 || (result < 0 && errno == EISCONN)) {
        complete_connect();
        return;
    }

    if (result < 0 && (errno == EINPROGRESS || errno == EINTR || errno == EALREADY)) {
        state_ = State::connecting;
        channel_ = std::make_shared<Channel>(loop_, socket_->file_descriptor());
        channel_->tie(shared_from_this());
        channel_->set_write_callback([this] { handle_write(); });
        channel_->set_error_callback([this] { handle_error(); });
        channel_->enable_writing();
        return;
    }

    fail_connect(std::error_code(errno, std::generic_category()));
}

void Connector::handle_write() {
    if (state_ != State::connecting) {
        return;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(socket_->file_descriptor(), SOL_SOCKET, SO_ERROR, &socket_error,
                     &error_size) < 0) {
        socket_error = errno;
    }
    retire_channel();
    if (socket_error != 0) {
        fail_connect(std::error_code(socket_error, std::generic_category()));
        return;
    }
    complete_connect();
}

void Connector::handle_error() {
    if (state_ != State::connecting) {
        return;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(socket_->file_descriptor(), SOL_SOCKET, SO_ERROR, &socket_error,
                     &error_size) < 0) {
        socket_error = errno;
    }
    retire_channel();
    fail_connect(std::error_code(socket_error == 0 ? ECONNABORTED : socket_error,
                                 std::generic_category()));
}

void Connector::complete_connect() {
    if (!connect_requested_ || !socket_) {
        return;
    }
    state_ = State::connected;
    Socket connected_socket = std::move(*socket_);
    socket_.reset();
    if (new_connection_callback_) {
        new_connection_callback_(std::move(connected_socket));
    }
}

void Connector::fail_connect(std::error_code error) {
    socket_.reset();
    state_ = State::disconnected;
    if (connect_requested_ && connect_error_callback_) {
        connect_error_callback_(error);
    }
}

void Connector::retire_channel() {
    if (!channel_) {
        return;
    }
    std::shared_ptr<Channel> retired = std::move(channel_);
    retired->disable_all();
    retired->remove();
    // Channel 正在执行回调时不能立即析构；延迟到本轮 epoll 分发结束后释放。
    loop_.queue_in_loop([retired] {});
}

}  // namespace cabinflow::net::detail
