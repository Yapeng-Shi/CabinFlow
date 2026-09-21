#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cabinflow::net::detail {

// TCP 连接的读写缓冲只属于对应 I/O 线程；并发访问必须经 EventLoop 交接。
class Buffer final {
public:
    explicit Buffer(std::size_t initial_size = 4096);

    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::size_t writable_bytes() const noexcept;
    [[nodiscard]] const char* peek() const noexcept;

    void retrieve(std::size_t length);
    void retrieve_all() noexcept;
    [[nodiscard]] std::string retrieve_all_as_string();

    void append(const char* data, std::size_t length);
    void append(const std::string& data);

    void append_uint32(std::uint32_t value);
    [[nodiscard]] std::uint32_t read_uint32();

private:
    void ensure_writable_bytes(std::size_t length);
    void make_space(std::size_t length);
    [[nodiscard]] char* begin() noexcept;
    [[nodiscard]] const char* begin() const noexcept;
    [[nodiscard]] char* begin_write() noexcept;

    static constexpr std::size_t kCheapPrepend = 8;
    std::vector<char> bytes_;
    std::size_t reader_index_{kCheapPrepend};
    std::size_t writer_index_{kCheapPrepend};
};

}  // namespace cabinflow::net::detail
