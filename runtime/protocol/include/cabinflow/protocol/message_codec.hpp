#pragma once

#include <string>

#include <cabinflow/protocol/message.hpp>
#include <cabinflow/protocol/protocol_error.hpp>

namespace cabinflow::protocol {

struct EncodeResult {
    std::string bytes;
    CodecError error{CodecError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == CodecError::kNone;
    }
};

struct DecodeResult {
    Message message;
    CodecError error{CodecError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == CodecError::kNone;
    }
};

[[nodiscard]] EncodeResult encode_message(const Message& message);
[[nodiscard]] DecodeResult decode_message(const std::string& bytes);

}  // namespace cabinflow::protocol
