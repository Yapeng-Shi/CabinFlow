#include "acceptor.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <cabinflow/net/event_loop.hpp>

namespace cabinflow::net::detail {

Acceptor::Acceptor(EventLoop& loop, const InetAddress& listen_address, bool reuse_port)
    : loop_(loop),
      listen_socket_(Socket::create_nonblocking(listen_address.family())),
      listen_channel_(loop, listen_socket_.file_descriptor()),
      idle_file_descriptor_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    if (idle_file_descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open(/dev/null)");
    }
    listen_socket_.set_reuse_address(true);
    listen_socket_.set_reuse_port(reuse_port);
    listen_socket_.bind(listen_address);
    listen_channel_.set_read_callback([this] { handle_read(); });
}

Acceptor::~Acceptor() {
    if (listening_) {
        std::terminate();
    }
    if (idle_file_descriptor_ >= 0) {
        static_cast<void>(::close(idle_file_descriptor_));
    }
}

void Acceptor::set_new_connection_callback(NewConnectionCallback callback) {
    assert_in_loop_thread();
    if (listening_) {
        throw std::logic_error("Acceptor callback must be set before listen");
    }
    new_connection_callback_ = std::move(callback);
}

void Acceptor::listen() {
    assert_in_loop_thread();
    if (listening_) {
        throw std::logic_error("Acceptor is already listening");
    }
    listen_socket_.listen();
    listening_ = true;
    listen_channel_.enable_reading();
}

void Acceptor::stop() {
    assert_in_loop_thread();
    if (!listening_) {
        return;
    }
    listening_ = false;
    listen_channel_.disable_all();
    listen_channel_.remove();
}

bool Acceptor::listening() const noexcept { return listening_; }

std::uint16_t Acceptor::bound_port() const {
    assert_in_loop_thread();
    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    if (::getsockname(listen_socket_.file_descriptor(),
                      reinterpret_cast<sockaddr*>(&address), &address_length) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockname");
    }
    return InetAddress::from_sockaddr(address).port();
}

void Acceptor::assert_in_loop_thread() const {
    if (!loop_.is_in_loop_thread()) {
        throw std::logic_error("Acceptor accessed outside EventLoop owner thread");
    }
}

void Acceptor::handle_read() {
    assert_in_loop_thread();
    while (true) {
        InetAddress peer_address = InetAddress::loopback(0);
        try {
            auto accepted = listen_socket_.accept(&peer_address);
            if (!accepted.has_value()) {
                return;
            }
            if (new_connection_callback_) {
                new_connection_callback_(std::move(*accepted), peer_address);
            }
        } catch (const std::system_error& error) {
            if (error.code().value() == EMFILE) {
                handle_file_descriptor_exhaustion();
                return;
            }
            throw;
        }
    }
}

void Acceptor::handle_file_descriptor_exhaustion() {
    // 预留一个空闲 FD，耗尽时丢弃一个排队连接，避免 epoll 持续报告同一监听事件。
    static_cast<void>(::close(idle_file_descriptor_));
    idle_file_descriptor_ = ::accept4(listen_socket_.file_descriptor(), nullptr, nullptr,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (idle_file_descriptor_ >= 0) {
        static_cast<void>(::close(idle_file_descriptor_));
    }
    idle_file_descriptor_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (idle_file_descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open(/dev/null)");
    }
}

}  // namespace cabinflow::net::detail
