#include "grpc_connection.hpp"

#include <cabinflow/gateway/message_limits.hpp>
#include <cabinflow/protocol/message_codec.hpp>
#include <grpc/impl/codegen/grpc_types.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using cabinflow::gateway::GrpcConnectionPtr;
using cabinflow::protocol::Message;
using cabinflow::protocol::v1::RuntimeMessage;

void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }

Message message(std::uint64_t sequence = 0) {
    Message result;
    result.envelope.message_id = "stream-contract-" + std::to_string(sequence);
    result.envelope.trace_id = "stream-contract";
    result.envelope.source_node = "worker";
    result.envelope.target_node = "client";
    result.envelope.topic = "contract.response";
    result.envelope.sequence = sequence;
    return result;
}

class Rendezvous final {
public:
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_; changed_.notify_all();
        require(changed_.wait_for(lock, 2s, [this] { return arrived_ == 3; }), "worker barrier timeout");
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned arrived_{0};
};

class Server final {
public:
    Server() : service_("stream-contract",
        [this](const GrpcConnectionPtr& connection, const Message&) {
            { std::lock_guard<std::mutex> lock(mutex_); connection_ = connection; }
            changed_.notify_all();
        }, [this](const GrpcConnectionPtr& connection) {
            if (connection->connected()) {
                std::lock_guard<std::mutex> lock(mutex_); ++opened_; return;
            }
            { std::lock_guard<std::mutex> lock(mutex_); ++closed_; }
            changed_.notify_all();
        }) {
        grpc::ServerBuilder builder;
        int bound_port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &bound_port);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_BDP_PROBE, 0);
        builder.RegisterService(&service_);
        builder.SetMaxSendMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        server_ = builder.BuildAndStart();
        require(static_cast<bool>(server_), "start real gRPC contract server");
        port_ = static_cast<std::uint16_t>(bound_port);
    }
    ~Server() { shutdown(); }
    void shutdown() {
        if (!server_) return;
        server_->Shutdown(std::chrono::system_clock::now() + 1s);
        server_->Wait();
        server_.reset();
    }
    std::uint16_t port() const noexcept { return port_; }
    GrpcConnectionPtr connection() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 2s, [this] { return static_cast<bool>(connection_); }),
                "request proves reactor is attached");
        return connection_;
    }
    void wait_closed() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 2s, [this] { return closed_ != 0; }), "OnDone timeout");
        require(opened_ == 1 && closed_ == 1, "one stream produces exactly one OnDone, without retry");
    }
    void release_connection() { std::lock_guard<std::mutex> lock(mutex_); connection_.reset(); }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    GrpcConnectionPtr connection_;
    unsigned opened_{0}, closed_{0};
    cabinflow::gateway::GrpcService service_;
    std::unique_ptr<grpc::Server> server_;
    std::uint16_t port_{0};
};

class Client final {
public:
    explicit Client(std::uint16_t port) {
        grpc::ChannelArguments arguments;
        arguments.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 0);
        arguments.SetInt(GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES, 0);
        arguments.SetMaxReceiveMessageSize(static_cast<int>(cabinflow::gateway::kMaxMessageBytes));
        channel_ = grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
            grpc::InsecureChannelCredentials(), arguments);
        require(channel_->WaitForConnected(std::chrono::system_clock::now() + 2s), "channel timeout");
        stub_ = cabinflow::protocol::v1::RuntimeGateway::NewStub(channel_);
        context_.set_deadline(std::chrono::system_clock::now() + 10s);
        stream_ = stub_->Connect(&context_);
        const auto encoded = cabinflow::protocol::encode_message(message());
        RuntimeMessage seed;
        require(encoded && seed.ParseFromString(encoded.bytes) && stream_->Write(seed), "send initial request");
    }
    ~Client() { if (!finished_) { context_.TryCancel(); static_cast<void>(stream_->Finish()); } }
    void cancel() { context_.TryCancel(); }
    bool read(Message* result) {
        RuntimeMessage wire;
        if (!stream_->Read(&wire)) return false;
        auto decoded = cabinflow::protocol::decode_message(wire.SerializeAsString());
        require(static_cast<bool>(decoded), "response codec");
        *result = std::move(decoded.message); return true;
    }
    grpc::Status finish() { finished_ = true; return stream_->Finish(); }
private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<cabinflow::protocol::v1::RuntimeGateway::Stub> stub_;
    grpc::ClientContext context_;
    std::unique_ptr<grpc::ClientReaderWriter<RuntimeMessage, RuntimeMessage>> stream_;
    bool finished_{false};
};

void output_capacity_includes_in_flight() {
    require(cabinflow::gateway::kMaxStreamOutputMessages == 8, "external stream queue contract is eight");
    Server server; Client client(server.port()); const auto connection = server.connection();
    auto output = message(); output.payload.assign(3 * 1024 * 1024, 'x');
    // 客户端禁用预读与 BDP 扩窗且尚未 Read；大于 HTTP/2 初始窗口的首写留在 in-flight。
    for (std::size_t index = 0; index < cabinflow::gateway::kMaxStreamOutputMessages; ++index) {
        output.envelope.sequence = index;
        require(connection->send(output), "first eight outputs fit including in-flight write");
    }
    output.envelope.sequence = cabinflow::gateway::kMaxStreamOutputMessages;
    require(!connection->send(output) && !connection->connected(), "ninth output overflows and closes admission");
    for (int index = 0; index < 3; ++index) require(!connection->send(message()), "closed output never retries");
    Message response; unsigned received = 0;
    while (client.read(&response)) {
        require(++received == 1 && response.envelope.sequence == 0 && response.payload == output.payload,
                "only the retained in-flight message may finish; queued outputs are discarded");
    }
    require(client.finish().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED,
            "overflow terminates with RESOURCE_EXHAUSTED");
    server.wait_closed();
}

void same_topic_worker_order() {
    Server server; Client client(server.port()); const auto connection = server.connection();
    Rendezvous barrier; std::array<std::exception_ptr, 2> errors{};
    std::uint64_t next_sequence = 0;
    auto worker = [&](unsigned index) {
        try {
            barrier.arrive();
            for (unsigned count = 0; count < 2; ++count) {
                // sequence 分配与队列提交共同受 response_mutex 保护，两个 worker 不可交换顺序。
                std::lock_guard<std::mutex> lock(connection->response_mutex);
                require(connection->send(message(next_sequence++)), "ordered output admitted");
            }
        } catch (...) { errors[index] = std::current_exception(); }
    };
    std::thread first(worker, 0), second(worker, 1);
    try { barrier.arrive(); } catch (...) { first.join(); second.join(); throw; }
    first.join(); second.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    connection->shutdown();
    Message response;
    for (unsigned index = 0; index < 4; ++index)
        require(client.read(&response) && response.envelope.topic == "contract.response" &&
                response.envelope.sequence == index, "same-topic worker responses preserve assigned sequence");
    require(!client.read(&response) && client.finish().ok(), "four outputs end once without duplicate replay");
    server.wait_closed();
}

void concurrent_worker_send_and_disconnect() {
    for (unsigned iteration = 0; iteration < 20; ++iteration) {
        std::weak_ptr<cabinflow::gateway::GrpcConnection> weak;
        {
        Server server; Client client(server.port()); auto connection = server.connection();
        Rendezvous barrier; std::array<std::exception_ptr, 2> errors{};
        auto worker = [&](unsigned index) {
            try { barrier.arrive(); static_cast<void>(connection->send(message(index))); }
            catch (...) { errors[index] = std::current_exception(); }
        };
        std::thread first(worker, 0), second(worker, 1);
        try { barrier.arrive(); client.cancel(); } catch (...) { first.join(); second.join(); throw; }
        first.join(); second.join();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
        Message ignored; while (client.read(&ignored)) {}
        require(client.finish().error_code() == grpc::StatusCode::CANCELLED, "disconnect reaches client terminal");
        server.wait_closed();
        require(!connection->connected(), "OnDone detaches reactor before worker observes closed");
        for (unsigned index = 0; index < 8; ++index)
            require(!connection->send(message(index)), "late worker cannot access or revive detached reactor");
        weak = connection;
        server.release_connection(); connection.reset();
        }
        require(weak.expired(), "connection ownership releases after OnDone and worker completion");
    }
}

void concurrent_worker_send_and_server_shutdown() {
    for (unsigned iteration = 0; iteration < 5; ++iteration) {
        std::weak_ptr<cabinflow::gateway::GrpcConnection> weak;
        {
            Server server; Client client(server.port()); auto connection = server.connection();
            auto output = message(); output.payload.assign(3 * 1024 * 1024, 's');
            // 禁用预读且不调用 Read，使首写在途；覆盖 Shutdown 与持有消息的写回调交错。
            require(connection->send(output), "shutdown fixture admits in-flight output");
            Rendezvous barrier; std::array<std::exception_ptr, 2> errors{};
            auto worker = [&](unsigned index) {
                try { barrier.arrive(); static_cast<void>(connection->send(message(index + 1))); }
                catch (...) { errors[index] = std::current_exception(); }
            };
            std::thread first(worker, 0), second(worker, 1);
            try { barrier.arrive(); server.shutdown(); }
            catch (...) { first.join(); second.join(); throw; }
            first.join(); second.join();
            for (const auto& error : errors) if (error) std::rethrow_exception(error);
            Message response;
            while (client.read(&response)) {}
            require(!client.finish().ok(), "server shutdown terminates the unfinished stream");
            server.wait_closed();
            require(!connection->connected(), "server shutdown detaches reactor before returning");
            for (unsigned index = 0; index < 8; ++index)
                require(!connection->send(message(index)), "late worker cannot revive a stopped server");
            weak = connection;
            server.release_connection(); connection.reset();
            server.shutdown();
        }
        require(weak.expired(), "server shutdown releases connection after worker completion");
    }
}
} // namespace

int main() {
    try {
        output_capacity_includes_in_flight();
        same_topic_worker_order();
        concurrent_worker_send_and_disconnect();
        concurrent_worker_send_and_server_shutdown();
        std::cout << "grpc_stream_contract_test passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "grpc_stream_contract_test failed: " << error.what() << '\n'; return 1;
    }
}
