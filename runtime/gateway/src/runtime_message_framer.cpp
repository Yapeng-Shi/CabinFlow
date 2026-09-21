#include <cabinflow/gateway/runtime_message_framer.hpp>

#include <utility>

#include <cabinflow/protocol/message_codec.hpp>

namespace cabinflow::gateway {
namespace {

std::uint32_t read_big_endian_u32(const std::string& bytes) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]));
}

void append_big_endian_u32(std::string& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<char>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<char>(value & 0xffU));
}

}  // namespace

FrameEncodeResult RuntimeMessageFramer::encode(const protocol::Message& message) {
    const auto encoded = protocol::encode_message(message);
    if (!encoded) {
        return {{}, FrameError::kSerializationFailure};
    }

    if (encoded.bytes.empty()) {
        return {{}, FrameError::kZeroBody};
    }
    if (encoded.bytes.size() > kMaxFrameBytes) {
        return {{}, FrameError::kBodyTooLarge};
    }

    std::string frame;
    frame.reserve(kFrameHeaderBytes + encoded.bytes.size());
    append_big_endian_u32(frame, static_cast<std::uint32_t>(encoded.bytes.size()));
    frame.append(encoded.bytes);
    return {std::move(frame), FrameError::kNone};
}

FrameFeedResult RuntimeMessageFramer::feed(std::string_view bytes) {
    if (terminal_error_ != FrameError::kNone) {
        return {{}, terminal_error_};
    }

    if (!bytes.empty()) {
        pending_bytes_.append(bytes.data(), bytes.size());
    }
    FrameFeedResult result;

    while (true) {
        if (pending_bytes_.size() < kFrameHeaderBytes) {
            return result;
        }

        const auto body_size = read_big_endian_u32(pending_bytes_);
        if (body_size == 0U) {
            terminal_error_ = FrameError::kZeroBody;
            return {std::move(result.messages), terminal_error_};
        }
        if (body_size > kMaxFrameBytes) {
            terminal_error_ = FrameError::kBodyTooLarge;
            return {std::move(result.messages), terminal_error_};
        }

        const auto frame_size = kFrameHeaderBytes + static_cast<std::size_t>(body_size);
        if (pending_bytes_.size() < frame_size) {
            return result;
        }

        // 只有完整帧体才移交 Protobuf；半包绝不能触发猜测式解析。
        const std::string body = pending_bytes_.substr(kFrameHeaderBytes, body_size);
        const auto decoded = protocol::decode_message(body);
        if (!decoded) {
            terminal_error_ = FrameError::kMalformedProtobuf;
            return {std::move(result.messages), terminal_error_};
        }

        pending_bytes_.erase(0, frame_size);
        result.messages.push_back(decoded.message);
    }
}

}  // namespace cabinflow::gateway
