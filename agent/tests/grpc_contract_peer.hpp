#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <runtime_gateway.grpc.pb.h>
#include <cabinflow/gateway/message_limits.hpp>
#include <cabinflow/protocol/message_codec.hpp>

// 假后端仍通过真实 gRPC 流收发；读写分别由服务线程持有，Qt 线程只访问队列。
class GrpcContractPeer final : public cabinflow::protocol::v1::RuntimeGateway::Service {
public:
    GrpcContractPeer() {
        grpc::ServerBuilder builder;
        int bound_port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port);
        builder.SetMaxReceiveMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        builder.SetMaxSendMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        builder.RegisterService(this);
        server_ = builder.BuildAndStart();
        if (!server_) throw std::runtime_error("start gRPC contract peer");
        port_ = static_cast<std::uint16_t>(bound_port);
    }
    ~GrpcContractPeer() {
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server_->Wait();
    }
    std::uint16_t port() const noexcept { return port_; }
    bool connected() const { std::lock_guard<std::mutex> lock(mutex_); return context_ != nullptr; }
    unsigned accepted_streams() const { std::lock_guard<std::mutex> lock(mutex_); return streams_; }
    bool empty() const { std::lock_guard<std::mutex> lock(mutex_); return incoming_.empty(); }
    bool ready() const { std::lock_guard<std::mutex> lock(mutex_); return !incoming_.empty() || decode_failed_; }
    bool drained() const { std::lock_guard<std::mutex> lock(mutex_); return outgoing_.empty() && !writing_; }
    cabinflow::protocol::Message take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (decode_failed_ || incoming_.empty()) throw std::runtime_error("invalid or missing gRPC client request");
        auto result = std::move(incoming_.front()); incoming_.pop_front(); return result;
    }
    void send(const cabinflow::protocol::Message& message) {
        const auto encoded = cabinflow::protocol::encode_message(message);
        cabinflow::protocol::v1::RuntimeMessage wire;
        if (!encoded || !wire.ParseFromString(encoded.bytes)) throw std::runtime_error("encode gRPC contract response");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!context_) throw std::runtime_error("gRPC contract stream unavailable");
            outgoing_.push_back(std::move(wire));
        }
        changed_.notify_all();
    }
    void disconnect() { std::lock_guard<std::mutex> lock(mutex_); if (context_) context_->TryCancel(); }
private:
    grpc::Status Connect(grpc::ServerContext* context,
        grpc::ServerReaderWriter<cabinflow::protocol::v1::RuntimeMessage,
                                 cabinflow::protocol::v1::RuntimeMessage>* stream) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++streams_;
            if (context_) return {grpc::StatusCode::ALREADY_EXISTS, "one contract stream only"};
            context_ = context;
        }
        bool stopped = false;
        std::thread writer([&] {
            while (true) {
                cabinflow::protocol::v1::RuntimeMessage wire;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait(lock, [&] { return stopped || !outgoing_.empty(); });
                    if (stopped) break;
                    wire = std::move(outgoing_.front()); outgoing_.pop_front(); writing_ = true;
                }
                const bool sent = stream->Write(wire);
                {
                    std::lock_guard<std::mutex> lock(mutex_); writing_ = false;
                }
                if (!sent) { context->TryCancel(); break; }
            }
        });
        cabinflow::protocol::v1::RuntimeMessage wire;
        while (stream->Read(&wire)) {
            auto decoded = cabinflow::protocol::decode_message(wire.SerializeAsString());
            std::lock_guard<std::mutex> lock(mutex_);
            if (!decoded) { decode_failed_ = true; break; }
            incoming_.push_back(std::move(decoded.message));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_); stopped = true;
        }
        changed_.notify_all(); context->TryCancel(); writer.join();
        {
            std::lock_guard<std::mutex> lock(mutex_); context_ = nullptr;
        }
        return {grpc::StatusCode::CANCELLED, "contract peer closed"};
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<cabinflow::protocol::Message> incoming_;
    std::deque<cabinflow::protocol::v1::RuntimeMessage> outgoing_;
    bool decode_failed_{false}, writing_{false};
    unsigned streams_{0};
    grpc::ServerContext* context_{nullptr};
    std::uint16_t port_{0};
    std::unique_ptr<grpc::Server> server_;
};
