#include "buffer.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cabinflow::net::detail {

Buffer::Buffer(std::size_t initial_size)
    : bytes_(kCheapPrepend + initial_size) {}

std::size_t Buffer::readable_bytes() const noexcept {
    return writer_index_ - reader_index_;
}

std::size_t Buffer::writable_bytes() const noexcept {
    return bytes_.size() - writer_index_;
}

const char* Buffer::peek() const noexcept { return begin() + reader_index_; }

void Buffer::retrieve(std::size_t length) {
    if (length > readable_bytes()) {
        throw std::out_of_range("Buffer retrieve exceeds readable bytes");
    }
    if (length == readable_bytes()) {
        retrieve_all();
        return;
    }
    reader_index_ += length;
}

void Buffer::retrieve_all() noexcept {
    reader_index_ = kCheapPrepend;
    writer_index_ = kCheapPrepend;
}

std::string Buffer::retrieve_all_as_string() {
    std::string result(peek(), readable_bytes());
    retrieve_all();
    return result;
}

void Buffer::append(const char* data, std::size_t length) {
    if (data == nullptr && length != 0) {
        throw std::invalid_argument("Buffer append data must not be null");
    }
    if (length == 0) {
        return;
    }
    ensure_writable_bytes(length);
    std::copy(data, data + length, begin_write());
    writer_index_ += length;
}

void Buffer::append(const std::string& data) { append(data.data(), data.size()); }

void Buffer::append_uint32(std::uint32_t value) {
    const std::uint32_t network_value = htonl(value);
    append(reinterpret_cast<const char*>(&network_value), sizeof(network_value));
}

std::uint32_t Buffer::read_uint32() {
    if (readable_bytes() < sizeof(std::uint32_t)) {
        throw std::out_of_range("Buffer does not contain uint32");
    }
    std::uint32_t network_value = 0;
    std::memcpy(&network_value, peek(), sizeof(network_value));
    retrieve(sizeof(network_value));
    return ntohl(network_value);
}

void Buffer::ensure_writable_bytes(std::size_t length) {
    if (writable_bytes() < length) {
        make_space(length);
    }
}

void Buffer::make_space(std::size_t length) {
    if (writable_bytes() + reader_index_ < length + kCheapPrepend) {
        bytes_.resize(writer_index_ + length);
        return;
    }

    const std::size_t readable = readable_bytes();
    std::copy(begin() + reader_index_, begin() + writer_index_,
              begin() + kCheapPrepend);
    reader_index_ = kCheapPrepend;
    writer_index_ = reader_index_ + readable;
}

char* Buffer::begin() noexcept { return bytes_.data(); }

const char* Buffer::begin() const noexcept { return bytes_.data(); }

char* Buffer::begin_write() noexcept { return begin() + writer_index_; }

}  // namespace cabinflow::net::detail
