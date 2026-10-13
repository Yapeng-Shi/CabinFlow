#include "grpc_connection.hpp"
#include <cabinflow/gateway/message_limits.hpp>
#include <cabinflow/protocol/message_codec.hpp>

namespace cabinflow::gateway {
class GrpcReactor final : public grpc::ServerBidiReactor<protocol::v1::RuntimeMessage, protocol::v1::RuntimeMessage> {
public:
    GrpcReactor(GrpcConnectionPtr connection, GrpcConnection::MessageHandler on_message,
                GrpcConnection::CloseHandler on_connection)
        : connection_(std::move(connection)), on_message_(std::move(on_message)),
          on_connection_(std::move(on_connection)) {
        connection_->reactor_ = this;
        StartRead(&input_);
    }
    void OnReadDone(bool ok) override {
        if (!ok) {
            connection_->shutdown();
            return;
        }
        // gRPC 负责拆帧；统一 codec 负责枚举及通用 Envelope 表示转换。
        const auto decoded = protocol::decode_message(input_.SerializeAsString());
        if (!decoded || input_.ByteSizeLong() > kMaxMessageBytes) {
            connection_->force_close();
            return;
        }
        if (connection_->connected()) on_message_(connection_, decoded.message);
        std::lock_guard<std::mutex> lock(connection_->mutex_);
        if (!connection_->closing_ && !connection_->finished_) StartRead(&input_);
    }
    void OnWriteDone(bool ok) override {
        std::lock_guard<std::mutex> lock(connection_->mutex_);
        connection_->writing_ = false;
        connection_->output_.pop_front();
        if (!ok) connection_->close_locked(grpc::Status(grpc::StatusCode::CANCELLED, "stream write failed"), true);
        else connection_->advance_locked();
    }
    void OnCancel() override {
        std::lock_guard<std::mutex> lock(connection_->mutex_);
        connection_->close_locked(grpc::Status(grpc::StatusCode::CANCELLED, "stream cancelled"), true);
    }
    void OnDone() override {
        {
            // worker 与最终反应共用此锁；detach 后任何迟到输出都不能碰已销毁的 reactor。
            std::lock_guard<std::mutex> lock(connection_->mutex_);
            connection_->connected_.store(false);
            connection_->reactor_ = nullptr;
            connection_->output_.clear();
        }
        on_connection_(connection_);
        delete this;
    }
private:
    GrpcConnectionPtr connection_;
    GrpcConnection::MessageHandler on_message_;
    GrpcConnection::CloseHandler on_connection_;
    protocol::v1::RuntimeMessage input_;
};

GrpcConnection::GrpcConnection(std::string name) : name_(std::move(name)) {}
bool GrpcConnection::send(const protocol::Message& message) {
    const auto encoded = protocol::encode_message(message);
    protocol::v1::RuntimeMessage wire;
    if (!encoded || encoded.bytes.size() > kMaxMessageBytes || !wire.ParseFromString(encoded.bytes)) {
        force_close(); return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reactor_ || closing_ || finished_) return false;
    if (output_.size() >= kMaxStreamOutputMessages) {
        close_locked(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "stream output queue full"), true);
        return false;
    }
    output_.push_back(std::move(wire));
    advance_locked();
    return true;
}
void GrpcConnection::advance_locked() {
    if (!reactor_ || finished_ || writing_) return;
    if (!output_.empty()) {
        writing_ = true;
        reactor_->StartWrite(&output_.front());
    } else if (closing_) {
        // Finish 是最后一次写；必须等既有 StartWrite 的反应结束，不能与它并发。
        finished_ = true;
        reactor_->Finish(terminal_status_);
    }
}
void GrpcConnection::close_locked(grpc::Status status, bool discard) {
    if (!closing_) terminal_status_ = std::move(status);
    closing_ = true;
    connected_.store(false);
    if (discard) {
        // 在途 protobuf 必须保留到 OnWriteDone；尚未开始的输出可以释放。
        while (output_.size() > (writing_ ? 1U : 0U)) output_.pop_back();
    }
    advance_locked();
}
void GrpcConnection::force_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid RuntimeMessage"), true);
}
void GrpcConnection::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked(grpc::Status::OK, false);
}
GrpcService::GrpcService(std::string name, GrpcConnection::MessageHandler on_message,
                         GrpcConnection::CloseHandler on_connection)
    : name_(std::move(name)), on_message_(std::move(on_message)), on_connection_(std::move(on_connection)) {}
grpc::ServerBidiReactor<protocol::v1::RuntimeMessage, protocol::v1::RuntimeMessage>*
GrpcService::Connect(grpc::CallbackServerContext*) {
    auto connection = std::make_shared<GrpcConnection>(name_ + ":" + std::to_string(next_id_.fetch_add(1)));
    on_connection_(connection);
    return new GrpcReactor(std::move(connection), on_message_, on_connection_);
}
}
