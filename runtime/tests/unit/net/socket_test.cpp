#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <iostream>
#include <stdexcept>
#include <system_error>

#include "socket.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_socket_is_nonblocking_and_close_on_exec() {
    const auto socket = cabinflow::net::detail::Socket::create_nonblocking(AF_INET);
    const int status_flags = ::fcntl(socket.file_descriptor(), F_GETFL);
    const int descriptor_flags = ::fcntl(socket.file_descriptor(), F_GETFD);
    require(status_flags >= 0 && (status_flags & O_NONBLOCK) != 0,
            "socket is not nonblocking");
    require(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
            "socket is not close-on-exec");
}

void test_listen_accept_and_raii_close() {
    auto listener = cabinflow::net::detail::Socket::create_nonblocking(AF_INET);
    listener.set_reuse_address(true);
    listener.bind(cabinflow::net::detail::InetAddress::loopback(0));
    listener.listen();

    sockaddr_in listening_address{};
    socklen_t listening_address_length = sizeof(listening_address);
    if (::getsockname(listener.file_descriptor(),
                      reinterpret_cast<sockaddr*>(&listening_address),
                      &listening_address_length) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockname");
    }

    cabinflow::net::detail::Socket client(
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (client.file_descriptor() < 0) {
        throw std::system_error(errno, std::generic_category(), "client socket");
    }
    if (::connect(client.file_descriptor(),
                  reinterpret_cast<const sockaddr*>(&listening_address),
                  sizeof(listening_address)) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }

    pollfd ready{listener.file_descriptor(), POLLIN, 0};
    require(::poll(&ready, 1, 500) == 1 && (ready.revents & POLLIN) != 0,
            "listener did not receive the connection");

    auto peer = cabinflow::net::detail::InetAddress::loopback(0);
    auto accepted = listener.accept(&peer);
    require(accepted.has_value(), "listener did not accept the connection");
    require(peer.family() == AF_INET && peer.ip() == "127.0.0.1" && peer.port() != 0,
            "accepted peer address is invalid");

    const int accepted_descriptor = accepted->file_descriptor();
    accepted.reset();
    errno = 0;
    require(::fcntl(accepted_descriptor, F_GETFD) == -1 && errno == EBADF,
            "accepted socket was not closed by RAII");
}

}  // namespace

int main() {
    try {
        test_socket_is_nonblocking_and_close_on_exec();
        test_listen_accept_and_raii_close();
        std::cout << "socket test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "socket test failed: " << error.what() << '\n';
        return 1;
    }
}
