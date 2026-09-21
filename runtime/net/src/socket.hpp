#pragma once

#include <sys/socket.h>

#include <optional>

#include "inet_address.hpp"

namespace cabinflow::net::detail {

class Socket final {
public:
    explicit Socket(int file_descriptor) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Socket create_nonblocking(sa_family_t family);

    [[nodiscard]] int file_descriptor() const noexcept;
    [[nodiscard]] int release() noexcept;
    void bind(const InetAddress& address);
    void listen();
    [[nodiscard]] std::optional<Socket> accept(InetAddress* peer_address);
    void shutdown_write();
    void set_reuse_address(bool enabled);
    void set_reuse_port(bool enabled);
    void set_keep_alive(bool enabled);
    void set_tcp_no_delay(bool enabled);

private:
    void set_option(int level, int option, bool enabled);

    int file_descriptor_{-1};
};

}  // namespace cabinflow::net::detail
