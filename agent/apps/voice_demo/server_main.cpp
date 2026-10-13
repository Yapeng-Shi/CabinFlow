#include <cabinflow/agent/inference/native_backends.hpp>
#include <cabinflow/agent/voice_pipeline.hpp>
#include <cabinflow/gateway/control_gateway.hpp>
#include <cabinflow/agent/socket_can_vehicle.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <pthread.h>
#include <signal.h>

int main(int argc, char** argv) {
    try {
        if (argc != 9) {
            std::cerr << "usage: voice_demo_server LISTEN_IP PORT ASR_MODEL_DIR LLM_GGUF TTS_MODEL_DIR RAG_SITE_PACKAGES RAG_MODEL_DIR RAG_INDEX_DIR\n";
            return 2;
        }
        const std::string_view port_text(argv[2]);
        unsigned int port = 0;
        const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port > 65535)
            throw std::runtime_error("PORT must be an integer in [0,65535]");
        sigset_t signals;
        if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 || sigaddset(&signals, SIGTERM) != 0)
            throw std::runtime_error("cannot initialize shutdown signals");
        // 模型/Runtime worker 继承屏蔽；sigwait 线程负责取消和 drain，异步 signal handler 不碰对象。
        const int masked = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        if (masked != 0) throw std::runtime_error("cannot block shutdown signals: " + std::to_string(masked));
        // 启动包含虚拟 ECU 查询、模型加载、Runtime 装配和 gRPC 监听。
        const auto startup_started = std::chrono::steady_clock::now();
        auto vehicle = std::make_unique<cabinflow::agent::SocketCanVehicle>();
        const auto initial_vehicle = vehicle->query();
        if (initial_vehicle.error != cabinflow::agent::VehicleError::none)
            throw std::runtime_error("vcan0 ECU not ready: " + initial_vehicle.detail);
        cabinflow::agent::VoicePipeline pipeline(
            std::make_unique<cabinflow::agent::inference::SherpaAsrBackend>(argv[3]),
            std::make_unique<cabinflow::agent::inference::PythonRagBackend>(argv[6], argv[7], argv[8], 2),
            std::make_unique<cabinflow::agent::inference::LlamaBackend>(argv[4], 2, 2048, 128),
            std::make_unique<cabinflow::agent::inference::SherpaTtsBackend>(argv[5]),
            std::move(vehicle));
        // Gateway 活到 Pipeline drain 之后，根 completion 才能安全访问输出流。
        cabinflow::gateway::ControlGateway gateway("voice-demo", argv[1],
            static_cast<std::uint16_t>(port), pipeline.registry(), pipeline.runtime(), pipeline.clock(), 300'000);
        gateway.set_data_task_hooks(pipeline.hooks());
        try {
            gateway.start();
            const auto startup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - startup_started).count();
            std::cout << "voice_demo_server listening=" << argv[1] << ':' << gateway.bound_port()
                      << " startup_elapsed_ns=" << startup_elapsed_ns
                      << " startup_boundary=ecu_query_model_load_runtime_assembly_grpc_listen"
                      << " transport=grpc vehicle=vcan0 models=locked_cpu semantic_quality=separate human_listening=not_verified\n"
                      << std::flush;
            int received = 0;
            const int waited = sigwait(&signals, &received);
            if (waited != 0) throw std::runtime_error("sigwait failed: " + std::to_string(waited));
            gateway.stop_admission();
            pipeline.shutdown();
            gateway.stop();
        } catch (...) {
            gateway.stop_admission();
            pipeline.shutdown();
            gateway.stop();
            throw;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "voice_demo_server failed: " << error.what() << '\n';
        return 1;
    }
}
