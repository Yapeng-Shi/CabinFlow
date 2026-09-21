#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "buffer.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_split_and_coalesced_bytes_preserve_order() {
    cabinflow::net::detail::Buffer buffer(4);
    buffer.append("ab", 2);
    buffer.append("cdef", 4);
    require(buffer.retrieve_all_as_string() == "abcdef",
            "buffer did not preserve byte order");
}

void test_network_order_uint32_round_trip() {
    cabinflow::net::detail::Buffer buffer;
    buffer.append_uint32(0x12345678U);
    require(buffer.read_uint32() == 0x12345678U,
            "buffer did not preserve uint32 network order");
}

}  // namespace

int main() {
    try {
        test_split_and_coalesced_bytes_preserve_order();
        test_network_order_uint32_round_trip();
        std::cout << "buffer test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "buffer test failed: " << error.what() << '\n';
        return 1;
    }
}
