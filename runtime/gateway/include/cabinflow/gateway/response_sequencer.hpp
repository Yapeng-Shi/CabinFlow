#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace cabinflow::gateway {

enum class ResponseSequenceError {
    kNone,
    kMissingConnectionId,
    kExhausted,
};

struct ResponseSequenceResult {
    std::uint64_t sequence{0};
    ResponseSequenceError error{ResponseSequenceError::kNone};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ResponseSequenceError::kNone;
    }
};

// 为不同响应 topic 分配独立流序号；空 session 的流以连接身份隔离。
class ResponseSequencer final {
public:
    ResponseSequencer();
    ~ResponseSequencer();

    ResponseSequencer(const ResponseSequencer&) = delete;
    ResponseSequencer& operator=(const ResponseSequencer&) = delete;
    ResponseSequencer(ResponseSequencer&&) = delete;
    ResponseSequencer& operator=(ResponseSequencer&&) = delete;

    [[nodiscard]] ResponseSequenceResult next(
        std::string_view session_id, std::string_view work_id,
        std::string_view topic,
        std::string_view connection_id);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace cabinflow::gateway
