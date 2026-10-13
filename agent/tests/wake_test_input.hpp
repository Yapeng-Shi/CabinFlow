#pragma once
#include "wake_input.hpp"
#include <QTimer>

// 明确测试采音端口；不会创建设备或伪装真实 KWS。
class WakeTestInput final : public WakeInput {
public:
    quint64 epoch{0};
    int starts{0}, stops{0};
    bool hold_stop{false}, hold_close{false}, closed_state{false};
    void listen(quint64 value) override { epoch = value; ++starts; }
    void stop(quint64 value) override {
        epoch = value; ++stops;
        if (!hold_stop) QTimer::singleShot(0, this, [this, value] { emit settled(value); });
    }
    void requestClose() override {
        if (closed_state || hold_close) return;
        QTimer::singleShot(0, this, [this] { finishClose(); });
    }
    void finishClose() { closed_state = true; emit closed(); }
    bool isClosed() const override { return closed_state; }
};
