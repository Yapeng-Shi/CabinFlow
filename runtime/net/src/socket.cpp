#include "socket.hpp"

#include <netinet/tcp.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace cabinflow::net::detail {

Socket::Socket(int file_descriptor) noexcept : file_descriptor_(file_descriptor) {}

Socket::~Socket() {
    if (file_descriptor_ >= 0) {
        static_cast<void>(::close(file_descriptor_));
    }
}

Socket::Socket(Socket&& other) noexcept
    : file_descriptor_(std::exchange(other.file_descriptor_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (file_descriptor_ >= 0) {
            static_cast<void>(::close(file_descriptor_));
        }
        file_descriptor_ = std::exchange(other.file_descriptor_, -1);
    }
    return *this;
}

Socket Socket::create_nonblocking(sa_family_t family) {
    const int file_descriptor = ::socket(family,
                                         SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                         IPPROTO_TCP);
    if (file_descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    return Socket(file_descriptor);
}

int Socket::file_descriptor() const noexcept { return file_descriptor_; }

int Socket::release() noexcept { return std::exchange(file_descriptor_, -1); }

void Socket::bind(const InetAddress& address) {
    if (::bind(file_descriptor_, address.sockaddr_ptr(), address.sockaddr_length()) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
}

void Socket::listen() {
    if (::listen(file_descriptor_, SOMAXCONN) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
}

std::optional<Socket> Socket::accept(InetAddress* peer_address) {
    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    const int accepted = ::accept4(file_descriptor_, reinterpret_cast<sockaddr*>(&address),
                                   &address_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
            errno == ECONNABORTED) {
            return std::nullopt;
        }
        throw std::system_error(errno, std::generic_category(), "accept4");
    }
    if (peer_address != nullptr) {
        *peer_address = InetAddress::from_sockaddr(address);
    }
    return Socket(accepted);
}

void Socket::shutdown_write() {
    if (::shutdown(file_descriptor_, SHUT_WR) < 0) {
        throw std::system_error(errno, std::generic_category(), "shutdown(SHUT_WR)");
    }
}

void Socket::set_reuse_address(bool enabled) {
    set_option(SOL_SOCKET, SO_REUSEADDR, enabled);
}

void Socket::set_reuse_port(bool enabled) {
#ifdef SO_REUSEPORT
    set_option(SOL_SOCKET, SO_REUSEPORT, enabled);
#else
    if (enabled) {
        throw std::runtime_error("SO_REUSEPORT is unavailable on this platform");
    }
#endif
}

void Socket::set_keep_alive(bool enabled) {
    set_option(SOL_SOCKET, SO_KEEPALIVE, enabled);
}

void Socket::set_tcp_no_delay(bool enabled) {
    set_option(IPPROTO_TCP, TCP_NODELAY, enabled);
}

void Socket::set_option(int level, int option, bool enabled) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(file_descriptor_, level, option, &value, sizeof(value)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
}

}  // namespace cabinflow::net::detail
