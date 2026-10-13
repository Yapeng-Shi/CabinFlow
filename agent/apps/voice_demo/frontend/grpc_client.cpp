#include "grpc_client.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <grpcpp/grpcpp.h>
#include <runtime_gateway.grpc.pb.h>
#include <cabinflow/gateway/message_limits.hpp>

struct GrpcClient::State {
    enum class Operation { kStart, kRead, kWrite, kFinish };
    Operation start_tag{Operation::kStart}, read_tag{Operation::kRead}, write_tag{Operation::kWrite}, finish_tag{Operation::kFinish};
    GrpcClient& owner;
    quint64 generation;
    grpc::CompletionQueue cq;
    grpc::ClientContext context;
    std::unique_ptr<cabinflow::protocol::v1::RuntimeGateway::Stub> stub;
    std::unique_ptr<grpc::ClientAsyncReaderWriter<cabinflow::protocol::v1::RuntimeMessage, cabinflow::protocol::v1::RuntimeMessage>> stream;
    cabinflow::protocol::v1::RuntimeMessage input;
    grpc::Status status;
    std::mutex mutex;
    std::deque<cabinflow::protocol::v1::RuntimeMessage> output;
    bool started{false}, reading{false}, writing{false}, closing{false}, finishing{false};
    std::atomic<bool> online{false}, done{false};
    std::thread worker;

    State(GrpcClient& owner, quint64 generation, std::string endpoint)
        : owner(owner), generation(generation), worker([this, endpoint = std::move(endpoint)] { run(endpoint); }) {}
    ~State() { stop(); if (worker.joinable()) worker.join(); }
    void post(std::function<void(GrpcClient&)> action) {
        QMetaObject::invokeMethod(&owner, [target = &owner, current = generation, action = std::move(action)] {
            if (target->generation_ == current) action(*target);
        }, Qt::QueuedConnection);
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true; online.store(false); context.TryCancel();
    }
    bool send(cabinflow::protocol::v1::RuntimeMessage message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!online.load() || closing || output.size() >= cabinflow::gateway::kMaxStreamOutputMessages) return false;
        output.push_back(std::move(message));
        advance(); return true;
    }
    void advance() {
        // 所有提交由同一锁串行化；Write 完成前不能释放 front protobuf。
        if (!started || finishing) return;
        if (closing) {
            if (!reading && !writing) {
                finishing = true; stream->Finish(&status, &finish_tag);
            }
        } else if (!writing && !output.empty()) {
            writing = true; stream->Write(output.front(), &write_tag);
        }
    }
    void run(const std::string& endpoint) {
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
        args.SetMaxReceiveMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        args.SetMaxSendMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        auto channel = grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), args);
        // 连接等待只发生在网络线程，GUI 可继续取消/关闭。
        if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
            cq.Shutdown();
            post([](GrpcClient& target) { emit target.failed("gRPC 连接失败，不自动重连"); });
            done.store(true);
            post([](GrpcClient& target) { emit target.disconnectedFromBackend(); });
            return;
        }
        stub = cabinflow::protocol::v1::RuntimeGateway::NewStub(channel);
        {
            std::lock_guard<std::mutex> lock(mutex);
            stream = stub->PrepareAsyncConnect(&context, &cq);
            stream->StartCall(&start_tag);
        }
        void* tag = nullptr;
        bool ok = false;
        while (cq.Next(&tag, &ok)) {
            std::lock_guard<std::mutex> lock(mutex);
            switch (*static_cast<Operation*>(tag)) {
                case Operation::kStart:
                    started = true;
                    if (!ok) closing = true;
                    if (!closing) {
                        online.store(true);
                        post([](GrpcClient& target) { emit target.connectedToBackend(); });
                        reading = true; stream->Read(&input, &read_tag);
                    }
                    advance(); break;
                case Operation::kRead:
                    reading = false;
                    if (ok) {
                        const auto bytes = input.SerializeAsString();
                        post([packet = QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size()))](GrpcClient& target) {
                            emit target.messageReceived(packet);
                        });
                        if (!closing) { reading = true; stream->Read(&input, &read_tag); }
                    } else { closing = true; online.store(false); }
                    advance(); break;
                case Operation::kWrite:
                    writing = false;
                    output.pop_front();
                    if (!ok) { closing = true; online.store(false); }
                    advance(); break;
                case Operation::kFinish:
                    online.store(false);
                    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
                        post([detail = QString::fromStdString(status.error_message())](GrpcClient& target) { emit target.failed(detail); });
                    }
                    // Finish 标签已收齐，再关闭 CQ 并排空；context 与 protobuf 的生命期覆盖所有标签。
                    cq.Shutdown(); break;
            }
        }
        done.store(true);
        post([](GrpcClient& target) { emit target.disconnectedFromBackend(); });
    }
};
GrpcClient::GrpcClient(QObject* parent) : QObject(parent) {}
GrpcClient::~GrpcClient() { state_.reset(); }
bool GrpcClient::connected() const noexcept { return state_ && state_->online.load(); }
bool GrpcClient::settled() const noexcept { return !state_ || state_->done.load(); }
void GrpcClient::stop() { if (state_) state_->stop(); }
void GrpcClient::connectToHost(const QString& host, quint16 port) {
    if (!settled()) return;
    if ((host != "127.0.0.1" && host != "::1") || port == 0) {
        emit failed("仅支持显式 loopback gRPC 地址"); return;
    }
    state_.reset();
    const auto endpoint = (host == "::1" ? "[::1]" : host) + ":" + QString::number(port);
    state_ = std::make_unique<State>(*this, ++generation_, endpoint.toStdString());
}
bool GrpcClient::send(const QByteArray& bytes) {
    if (bytes.size() > static_cast<qsizetype>(cabinflow::gateway::kMaxMessageBytes)) return false;
    cabinflow::protocol::v1::RuntimeMessage message;
    if (!state_ || !message.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) return false;
    return state_->send(std::move(message));
}
