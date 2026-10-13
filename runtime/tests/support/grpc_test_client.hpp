#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <runtime_gateway.grpc.pb.h>
#include <cabinflow/gateway/message_limits.hpp>
#include <cabinflow/protocol/message_codec.hpp>

namespace cabinflow::test {

// Read 独占后台线程；测试线程通过有界等待观察消息及结束，禁止用阻塞 Read 检验无响应。
class GrpcTestClient final {
public:
    explicit GrpcTestClient(std::uint16_t port) {
        grpc::ChannelArguments arguments;
        arguments.SetMaxSendMessageSize(static_cast<int>(gateway::kMaxMessageBytes + 1024));
        arguments.SetMaxReceiveMessageSize(static_cast<int>(gateway::kMaxMessageBytes));
        channel_ = grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials(), arguments);
        if (!channel_->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(2)))
            throw std::runtime_error("gRPC channel connection timeout");
        stub_ = protocol::v1::RuntimeGateway::NewStub(channel_);
        context_.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        stream_ = stub_->Connect(&context_);
        reader_ = std::thread([this] {
            protocol::v1::RuntimeMessage wire;
            while (stream_->Read(&wire)) {
                auto decoded = protocol::decode_message(wire.SerializeAsString());
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!decoded) decode_failed_ = true;
                    else incoming_.push_back(std::move(decoded.message));
                }
                changed_.notify_all();
            }
            const auto status = stream_->Finish();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = status;
                closed_ = true;
            }
            changed_.notify_all();
        });
    }
    ~GrpcTestClient() { context_.TryCancel(); if (reader_.joinable()) reader_.join(); }
    GrpcTestClient(const GrpcTestClient&) = delete;
    GrpcTestClient& operator=(const GrpcTestClient&) = delete;

    void send(const protocol::Message& message) {
        const auto encoded = protocol::encode_message(message);
        protocol::v1::RuntimeMessage wire;
        if (!encoded || !wire.ParseFromString(encoded.bytes))
            throw std::runtime_error("encode gRPC RuntimeMessage");
        if (!stream_->Write(wire)) throw std::runtime_error("write gRPC RuntimeMessage");
    }
    // 非法枚举及超限输入必须直接构造 wire，避免被客户端正常 codec 预先拒绝。
    void send_wire(const protocol::v1::RuntimeMessage& wire) { static_cast<void>(stream_->Write(wire)); }
    protocol::Message read() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [this] {
                return !incoming_.empty() || closed_ || decode_failed_;
            }) || incoming_.empty() || decode_failed_)
            throw std::runtime_error("gRPC response timeout, invalid response or stream closed");
        auto result = std::move(incoming_.front()); incoming_.pop_front(); return result;
    }
    void no_response() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (changed_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !incoming_.empty() || closed_ || decode_failed_;
            })) throw std::runtime_error("unexpected gRPC acknowledgement, result or termination");
    }
    grpc::Status wait_closed() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [this] { return closed_; }))
            throw std::runtime_error("gRPC stream did not terminate");
        if (!incoming_.empty() || decode_failed_) throw std::runtime_error("unexpected response before stream termination");
        return status_;
    }
private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<protocol::v1::RuntimeGateway::Stub> stub_;
    grpc::ClientContext context_;
    std::unique_ptr<grpc::ClientReaderWriter<protocol::v1::RuntimeMessage, protocol::v1::RuntimeMessage>> stream_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<protocol::Message> incoming_;
    bool closed_{false}, decode_failed_{false};
    grpc::Status status_;
    std::thread reader_;
};
} // namespace cabinflow::test
