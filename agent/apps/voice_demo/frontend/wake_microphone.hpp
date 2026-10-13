#pragma once
#include "wake_input.hpp"
#include <QThread>
#include <atomic>

class WakeMicrophone final : public WakeInput {
    Q_OBJECT
public:
    explicit WakeMicrophone(QString model_directory);
    ~WakeMicrophone() override;
    void listen(quint64 epoch) override;
    void stop(quint64 epoch) override;
    void requestClose() override;
    bool isClosed() const override { return closed_; }
private:
    class Worker;
    QThread thread_;
    Worker* worker_;
    std::atomic<quint64> epoch_{0};
    std::atomic<bool> closing_{false};
    bool closed_{false};
};
