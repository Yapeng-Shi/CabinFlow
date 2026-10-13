#pragma once

#include <memory>
#include <functional>
#include <string>

// 前端私有的采音算法；不把 Sherpa 类型或唤醒语义放进通用 Runtime。
class WakeEngine final {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrameSamples = 512;
    enum class Event { kNone, kWake, kSpeech, kCommand, kCancelled, kFailed };
    struct Result {
        Event event{Event::kNone};
        std::string wav, error;
    };
    explicit WakeEngine(const std::string& model_directory);
    ~WakeEngine();
    WakeEngine(const WakeEngine&) = delete;
    WakeEngine& operator=(const WakeEngine&) = delete;
    void reset();
    Result accept(const float* samples, int count, const std::function<int()>& elapsed_ms);
    Result checkDeadline(int elapsed_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
