#pragma once
#include <atomic>
#include <memory>
#include <QByteArray>
#include <QObject>
#include <QString>

class GrpcClient final : public QObject {
    Q_OBJECT
public:
    explicit GrpcClient(QObject* parent = nullptr);
    ~GrpcClient() override;
    void connectToHost(const QString& host, quint16 port);
    bool send(const QByteArray& protobuf);
    void stop();
    bool connected() const noexcept;
    bool settled() const noexcept;
signals:
    void connectedToBackend();
    void messageReceived(QByteArray protobuf);
    void failed(QString detail);
    void disconnectedFromBackend();
private:
    struct State;
    std::unique_ptr<State> state_;
    quint64 generation_{0};
};
