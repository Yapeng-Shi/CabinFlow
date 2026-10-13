#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <grpcpp/grpcpp.h>
#include <runtime_gateway.grpc.pb.h>
#include <cabinflow/protocol/message.hpp>

namespace cabinflow::gateway {
class GrpcReactor;
class GrpcConnection final {
public:
    using MessageHandler = std::function<void(const std::shared_ptr<GrpcConnection>&, const protocol::Message&)>;
    using CloseHandler = std::function<void(const std::shared_ptr<GrpcConnection>&)>;
    explicit GrpcConnection(std::string name);
    bool connected() const noexcept { return connected_.load(); }
    const std::string& name() const noexcept { return name_; }
    bool send(const protocol::Message& message);
    void force_close();
    void shutdown();
    // 同一流内的“分配响应 sequence → 提交发送队列”必须是一个临界区。
    std::mutex response_mutex;
private:
    friend class GrpcReactor;
    void close_locked(grpc::Status status, bool discard);
    void advance_locked();
    std::string name_;
    std::atomic<bool> connected_{true};
    std::mutex mutex_;
    GrpcReactor* reactor_{nullptr};
    std::deque<protocol::v1::RuntimeMessage> output_;
    bool writing_{false}, closing_{false}, finished_{false};
    grpc::Status terminal_status_;
};
using GrpcConnectionPtr = std::shared_ptr<GrpcConnection>;

class GrpcService final : public protocol::v1::RuntimeGateway::CallbackService {
public:
    GrpcService(std::string name, GrpcConnection::MessageHandler on_message,
                GrpcConnection::CloseHandler on_connection);
    grpc::ServerBidiReactor<protocol::v1::RuntimeMessage, protocol::v1::RuntimeMessage>*
    Connect(grpc::CallbackServerContext* context) override;
private:
    std::string name_;
    GrpcConnection::MessageHandler on_message_;
    GrpcConnection::CloseHandler on_connection_;
    std::atomic<std::uint64_t> next_id_{0};
};
}
