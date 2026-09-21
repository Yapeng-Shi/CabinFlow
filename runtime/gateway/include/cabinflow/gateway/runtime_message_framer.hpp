#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cabinflow/protocol/message.hpp>

namespace cabinflow::gateway {

inline constexpr std::size_t kFrameHeaderBytes = 4;
inline constexpr std::uint32_t kMaxFrameBytes = 4 * 1024 * 1024;

enum class FrameError {
    kNone,
    kZeroBody,
    kBodyTooLarge,
    kMalformedProtobuf,
    kSerializationFailure,
};

struct FrameEncodeResult {
    std::string bytes;
    FrameError error{FrameError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == FrameError::kNone;
    }
};

struct FrameFeedResult {
    std::vector<protocol::Message> messages;
    FrameError error{FrameError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == FrameError::kNone;
    }
};

// 连接拥有者收到非 kNone 错误后必须关闭 TCP 连接；Framer 不提供 JSON 或重置路径。
class RuntimeMessageFramer final {
public:
    [[nodiscard]] static FrameEncodeResult encode(const protocol::Message& message);
    [[nodiscard]] FrameFeedResult feed(std::string_view bytes);

private:
    std::string pending_bytes_;
    FrameError terminal_error_{FrameError::kNone};
};

}  // namespace cabinflow::gateway
