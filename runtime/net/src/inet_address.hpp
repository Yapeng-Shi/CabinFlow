#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace cabinflow::net::detail {

class InetAddress final {
public:
    InetAddress(std::string_view ip, std::uint16_t port);
    static InetAddress loopback(std::uint16_t port, bool ipv6 = false);
    static InetAddress from_sockaddr(const sockaddr_storage& address);

    [[nodiscard]] sa_family_t family() const noexcept;
    [[nodiscard]] const sockaddr* sockaddr_ptr() const noexcept;
    [[nodiscard]] socklen_t sockaddr_length() const noexcept;
    [[nodiscard]] std::string ip() const;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string ip_port() const;

private:
    explicit InetAddress(const sockaddr_in& address);
    explicit InetAddress(const sockaddr_in6& address);

    sockaddr_storage address_{};
    socklen_t address_length_{0};
};

}  // namespace cabinflow::net::detail
