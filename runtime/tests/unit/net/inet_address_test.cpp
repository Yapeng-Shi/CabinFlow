#include <iostream>
#include <stdexcept>

#include "inet_address.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_ipv4_endpoint_round_trip() {
    const cabinflow::net::detail::InetAddress address("127.0.0.1", 4321);
    require(address.ip() == "127.0.0.1", "unexpected IPv4 address");
    require(address.port() == 4321, "unexpected IPv4 port");
    require(address.ip_port() == "127.0.0.1:4321", "unexpected IPv4 endpoint");
}

void test_ipv6_endpoint_round_trip() {
    const auto address = cabinflow::net::detail::InetAddress::loopback(4321, true);
    require(address.ip() == "::1", "unexpected IPv6 address");
    require(address.port() == 4321, "unexpected IPv6 port");
    require(address.ip_port() == "[::1]:4321", "unexpected IPv6 endpoint");
}

}  // namespace

int main() {
    try {
        test_ipv4_endpoint_round_trip();
        test_ipv6_endpoint_round_trip();
        std::cout << "inet address test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "inet address test failed: " << error.what() << '\n';
        return 1;
    }
}
