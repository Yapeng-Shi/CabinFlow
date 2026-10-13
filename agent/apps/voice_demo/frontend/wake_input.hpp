#pragma once
#include <QObject>
#include <QByteArray>

// 仅隔离真实麦克风边界，测试替身不接入生产；epoch 由 GUI 控制，废弃迟到结果。
class WakeInput : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void listen(quint64 epoch) = 0;
    virtual void stop(quint64 epoch) = 0;
    virtual void requestClose() = 0;
    virtual bool isClosed() const = 0;
signals:
    void listening(quint64 epoch, const QString& device);
    void awake(quint64 epoch);
    void speech(quint64 epoch);
    void wavReady(quint64 epoch, const QByteArray& wav);
    void failed(quint64 epoch, const QString& error);
    void cancelled(quint64 epoch, const QString& reason);
    void settled(quint64 epoch);
    void closed();
};
