#pragma once
#include "wake_engine.hpp"
#include <array>
#include <string>

// PCM 字节可能在一个16位样本中间拆开；只取完整帧，剩余字节留到下一次读取。
class PcmFrameBuffer final {
public:
    static constexpr std::size_t kCapacity = 32000;
    bool append(const char* bytes, std::size_t count) {
        if ((!bytes && count) || count > kCapacity - pending_.size()) return false;
        if (count) pending_.append(bytes, count);
        return true;
    }
    bool pop(std::array<float, WakeEngine::kFrameSamples>& frame) {
        constexpr std::size_t frame_bytes = WakeEngine::kFrameSamples * 2;
        if (pending_.size() < frame_bytes) return false;
        for (std::size_t i = 0; i < frame.size(); ++i) {
            const auto raw = static_cast<unsigned char>(pending_[i * 2]) |
                (static_cast<unsigned char>(pending_[i * 2 + 1]) << 8);
            frame[i] = (raw < 32768 ? raw : raw - 65536) / 32768.0F;
        }
        pending_.erase(0, frame_bytes);
        return true;
    }
    std::size_t size() const { return pending_.size(); }
    void clear() { pending_.clear(); }
private:
    std::string pending_;
};
