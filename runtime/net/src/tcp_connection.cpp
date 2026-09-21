#include <cabinflow/net/tcp_connection.hpp>

#include <sys/epoll.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

#include "buffer.hpp"
#include "channel.hpp"
#include "inet_address.hpp"
#include "socket.hpp"

namespace cabinflow::net {
namespace {

detail::InetAddress socket_endpoint(int file_descriptor, bool peer) {
    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    const int result = peer
                           ? ::getpeername(file_descriptor,
                                           reinterpret_cast<sockaddr*>(&address),
                                           &address_length)
                           : ::getsockname(file_descriptor,
                                           reinterpret_cast<sockaddr*>(&address),
                                           &address_length);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(),
                                peer ? "getpeername" : "getsockname");
    }
    return detail::InetAddress::from_sockaddr(address);
}

}  // namespace

struct TcpConnection::Impl {
    Impl(EventLoop& loop_arg, std::string name_arg, detail::Socket socket_arg,
         detail::InetAddress local_address_arg, detail::InetAddress peer_address_arg)
        : loop(loop_arg),
          name(std::move(name_arg)),
          socket(std::move(socket_arg)),
          channel(loop, socket.file_descriptor()),
          local_address(std::move(local_address_arg)),
          peer_address(std::move(peer_address_arg)),
          local_endpoint(local_address.ip_port()),
          peer_endpoint(peer_address.ip_port()) {}

    EventLoop& loop;
    std::string name;
    detail::Socket socket;
    detail::Channel channel;
    detail::InetAddress local_address;
    detail::InetAddress peer_address;
    std::string local_endpoint;
    std::string peer_endpoint;
    std::atomic<ConnectionState> state{ConnectionState::connecting};
    std::atomic<bool> reading{false};
    ConnectionCallback connection_callback;
    MessageCallback message_callback;
    CloseCallback close_callback;
    detail::Buffer input_buffer;
    detail::Buffer output_buffer;
    TcpConnectionPtr lifetime_guard;
};

TcpConnectionPtr TcpConnection::create(EventLoop& loop, std::string name,
                                       int connected_file_descriptor) {
    if (!loop.is_in_loop_thread()) {
        throw std::logic_error("TcpConnection must be created in EventLoop owner thread");
    }
    if (connected_file_descriptor < 0) {
        throw std::invalid_argument("TcpConnection requires a connected file descriptor");
    }

    detail::Socket socket(connected_file_descriptor);
    const detail::InetAddress local_address =
        socket_endpoint(socket.file_descriptor(), false);
    const detail::InetAddress peer_address =
        socket_endpoint(socket.file_descriptor(), true);
    return TcpConnectionPtr(new TcpConnection(std::make_unique<Impl>(
        loop, std::move(name), std::move(socket), local_address, peer_address)));
}

TcpConnection::~TcpConnection() = default;

TcpConnection::TcpConnection(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {
    impl_->channel.set_read_callback([this] { handle_read(); });
    impl_->channel.set_write_callback([this] { handle_write(); });
    impl_->channel.set_close_callback([this] { handle_close(); });
    impl_->channel.set_error_callback([this] { handle_close(); });
}

void TcpConnection::set_connection_callback(ConnectionCallback callback) {
    if (!impl_->loop.is_in_loop_thread() ||
        impl_->state.load() != ConnectionState::connecting) {
        throw std::logic_error("TcpConnection callbacks must be set before establish");
    }
    impl_->connection_callback = std::move(callback);
}

void TcpConnection::set_message_callback(MessageCallback callback) {
    if (!impl_->loop.is_in_loop_thread() ||
        impl_->state.load() != ConnectionState::connecting) {
        throw std::logic_error("TcpConnection callbacks must be set before establish");
    }
    impl_->message_callback = std::move(callback);
}

void TcpConnection::set_close_callback(CloseCallback callback) {
    if (!impl_->loop.is_in_loop_thread() ||
        impl_->state.load() != ConnectionState::connecting) {
        throw std::logic_error("TcpConnection callbacks must be set before establish");
    }
    impl_->close_callback = std::move(callback);
}

void TcpConnection::establish() {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop([self] { self->establish_in_loop(); });
}

void TcpConnection::send(std::string bytes) {
    if (impl_->state.load() != ConnectionState::connected) {
        throw std::logic_error("TcpConnection cannot send while disconnected");
    }
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop(
        [self, bytes = std::move(bytes)]() mutable { self->send_in_loop(std::move(bytes)); });
}

void TcpConnection::shutdown() {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.queue_in_loop([self] { self->shutdown_in_loop(); });
}

void TcpConnection::force_close() {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop([self] { self->force_close_in_loop(); });
}

void TcpConnection::start_reading() {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop([self] { self->start_reading_in_loop(); });
}

void TcpConnection::stop_reading() {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop([self] { self->stop_reading_in_loop(); });
}

void TcpConnection::set_tcp_no_delay(bool enabled) {
    const TcpConnectionPtr self = shared_from_this();
    impl_->loop.run_in_loop([self, enabled] { self->impl_->socket.set_tcp_no_delay(enabled); });
}

const std::string& TcpConnection::name() const noexcept { return impl_->name; }

const std::string& TcpConnection::local_endpoint() const noexcept {
    return impl_->local_endpoint;
}

const std::string& TcpConnection::peer_endpoint() const noexcept {
    return impl_->peer_endpoint;
}

ConnectionState TcpConnection::state() const noexcept { return impl_->state.load(); }

bool TcpConnection::connected() const noexcept {
    return state() == ConnectionState::connected;
}

bool TcpConnection::is_reading() const noexcept { return impl_->reading.load(); }

void TcpConnection::establish_in_loop() {
    if (!impl_->loop.is_in_loop_thread() ||
        impl_->state.load() != ConnectionState::connecting) {
        throw std::logic_error("TcpConnection establish has invalid state");
    }

    impl_->socket.set_keep_alive(true);
    // 建连后由连接自身持有引用，直到 close 回调完成才解除，避免 epoll 回调访问悬空对象。
    impl_->lifetime_guard = shared_from_this();
    impl_->channel.tie(impl_->lifetime_guard);
    impl_->state.store(ConnectionState::connected);
    impl_->reading.store(true);
    impl_->channel.enable_reading();
    if (impl_->connection_callback) {
        impl_->connection_callback(shared_from_this());
    }
}

void TcpConnection::send_in_loop(std::string bytes) {
    if (!impl_->loop.is_in_loop_thread()) {
        throw std::logic_error("TcpConnection send executed outside EventLoop owner thread");
    }
    if (bytes.empty() || impl_->state.load() == ConnectionState::disconnected) {
        return;
    }

    std::size_t written = 0;
    if ((impl_->channel.events() & EPOLLOUT) == 0 &&
        impl_->output_buffer.readable_bytes() == 0) {
        while (true) {
            const ssize_t result = ::send(impl_->socket.file_descriptor(), bytes.data(),
                                          bytes.size(), MSG_NOSIGNAL);
            if (result >= 0) {
                written = static_cast<std::size_t>(result);
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                handle_close();
            }
            break;
        }
    }

    if (impl_->state.load() == ConnectionState::disconnected || written == bytes.size()) {
        return;
    }
    impl_->output_buffer.append(bytes.data() + written, bytes.size() - written);
    if ((impl_->channel.events() & EPOLLOUT) == 0) {
        impl_->channel.enable_writing();
    }
}

void TcpConnection::shutdown_in_loop() {
    if (impl_->state.load() != ConnectionState::connected) {
        return;
    }
    impl_->state.store(ConnectionState::disconnecting);
    if (impl_->output_buffer.readable_bytes() == 0) {
        impl_->socket.shutdown_write();
    }
}

void TcpConnection::force_close_in_loop() {
    if (impl_->state.load() == ConnectionState::connected ||
        impl_->state.load() == ConnectionState::disconnecting) {
        handle_close();
    }
}

void TcpConnection::start_reading_in_loop() {
    if (impl_->state.load() == ConnectionState::disconnected) {
        return;
    }
    if (!impl_->reading.exchange(true)) {
        impl_->channel.enable_reading();
    }
}

void TcpConnection::stop_reading_in_loop() {
    if (impl_->state.load() == ConnectionState::disconnected) {
        return;
    }
    if (impl_->reading.exchange(false)) {
        impl_->channel.disable_reading();
    }
}

void TcpConnection::handle_read() {
    if (impl_->state.load() == ConnectionState::disconnected) {
        return;
    }

    std::array<char, 64 * 1024> bytes{};
    bool peer_closed = false;
    while (true) {
        const ssize_t result = ::recv(impl_->socket.file_descriptor(), bytes.data(),
                                      bytes.size(), 0);
        if (result > 0) {
            impl_->input_buffer.append(bytes.data(), static_cast<std::size_t>(result));
            continue;
        }
        if (result == 0) {
            peer_closed = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            peer_closed = true;
        }
        break;
    }

    if (impl_->input_buffer.readable_bytes() != 0) {
        const std::string payload = impl_->input_buffer.retrieve_all_as_string();
        if (impl_->message_callback) {
            impl_->message_callback(shared_from_this(), payload);
        }
    }
    if (peer_closed) {
        handle_close();
    }
}

void TcpConnection::handle_write() {
    if (impl_->state.load() == ConnectionState::disconnected ||
        impl_->output_buffer.readable_bytes() == 0) {
        return;
    }

    while (impl_->output_buffer.readable_bytes() != 0) {
        const ssize_t result = ::send(impl_->socket.file_descriptor(), impl_->output_buffer.peek(),
                                      impl_->output_buffer.readable_bytes(), MSG_NOSIGNAL);
        if (result > 0) {
            impl_->output_buffer.retrieve(static_cast<std::size_t>(result));
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            handle_close();
        }
        break;
    }

    if (impl_->state.load() == ConnectionState::disconnected ||
        impl_->output_buffer.readable_bytes() != 0) {
        return;
    }
    impl_->channel.disable_writing();
    if (impl_->state.load() == ConnectionState::disconnecting) {
        impl_->socket.shutdown_write();
    }
}

void TcpConnection::handle_close() {
    if (!impl_->loop.is_in_loop_thread() ||
        impl_->state.load() == ConnectionState::disconnected) {
        return;
    }

    impl_->state.store(ConnectionState::disconnected);
    impl_->reading.store(false);
    impl_->channel.disable_all();
    impl_->channel.remove();

    const TcpConnectionPtr self = shared_from_this();
    if (impl_->connection_callback) {
        impl_->connection_callback(self);
    }
    if (impl_->close_callback) {
        impl_->close_callback(self);
    }
    impl_->lifetime_guard.reset();
}

}  // namespace cabinflow::net
