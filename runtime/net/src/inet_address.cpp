#include "inet_address.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>

namespace cabinflow::net::detail {
namespace {

std::string to_ip(const sockaddr* address) {
    char buffer[INET6_ADDRSTRLEN]{};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            throw std::runtime_error("inet_ntop(AF_INET) failed");
        }
        return buffer;
    }
    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer)) == nullptr) {
            throw std::runtime_error("inet_ntop(AF_INET6) failed");
        }
        return buffer;
    }
    throw std::invalid_argument("unsupported address family");
}

}  // namespace

InetAddress::InetAddress(std::string_view ip, std::uint16_t port) {
    sockaddr_in ipv4{};
    if (::inet_pton(AF_INET, std::string(ip).c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        *this = InetAddress(ipv4);
        return;
    }

    sockaddr_in6 ipv6{};
    if (::inet_pton(AF_INET6, std::string(ip).c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        *this = InetAddress(ipv6);
        return;
    }
    throw std::invalid_argument("invalid IPv4 or IPv6 address");
}

InetAddress InetAddress::loopback(std::uint16_t port, bool ipv6) {
    return ipv6 ? InetAddress("::1", port) : InetAddress("127.0.0.1", port);
}

InetAddress InetAddress::from_sockaddr(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET) {
        return InetAddress(*reinterpret_cast<const sockaddr_in*>(&address));
    }
    if (address.ss_family == AF_INET6) {
        return InetAddress(*reinterpret_cast<const sockaddr_in6*>(&address));
    }
    throw std::invalid_argument("unsupported sockaddr family");
}

sa_family_t InetAddress::family() const noexcept { return address_.ss_family; }

const sockaddr* InetAddress::sockaddr_ptr() const noexcept {
    return reinterpret_cast<const sockaddr*>(&address_);
}

socklen_t InetAddress::sockaddr_length() const noexcept { return address_length_; }

std::string InetAddress::ip() const { return to_ip(sockaddr_ptr()); }

std::uint16_t InetAddress::port() const noexcept {
    if (family() == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&address_)->sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&address_)->sin6_port);
}

std::string InetAddress::ip_port() const {
    if (family() == AF_INET6) {
        return "[" + ip() + "]:" + std::to_string(port());
    }
    return ip() + ":" + std::to_string(port());
}

InetAddress::InetAddress(const sockaddr_in& address)
    : address_length_(sizeof(address)) {
    std::memcpy(&address_, &address, sizeof(address));
}

InetAddress::InetAddress(const sockaddr_in6& address)
    : address_length_(sizeof(address)) {
    std::memcpy(&address_, &address, sizeof(address));
}

}  // namespace cabinflow::net::detail
