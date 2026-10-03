#include <cabinflow/agent/inference/native_backends.hpp>
#include <cabinflow/agent/voice_pipeline.hpp>
#include <cabinflow/gateway/control_gateway.hpp>
#include <cabinflow/net/event_loop.hpp>

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
        if (argc != 6) {
            std::cerr << "usage: voice_demo_server LISTEN_IP PORT ASR_MODEL_DIR LLM_GGUF TTS_MODEL_DIR\n";
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
        // 启动总耗时包含模型加载、Runtime 装配和 TCP 监听，不是单独模型加载时间。
        const auto startup_started = std::chrono::steady_clock::now();
        cabinflow::agent::VoicePipeline pipeline(
            std::make_unique<cabinflow::agent::inference::SherpaAsrBackend>(argv[3]),
            std::make_unique<cabinflow::agent::inference::LlamaBackend>(argv[4], 2, 2048, 128),
            std::make_unique<cabinflow::agent::inference::SherpaTtsBackend>(argv[5]),
            std::make_unique<cabinflow::agent::FakeVehicle>(false, false));  // 明确空调关、左前窗关，不自动猜状态。
        // EventLoop/Gateway 在 main 线程创建、启停、销毁；Gateway 与 Pipeline 共用 Runtime/Registry。
        cabinflow::net::EventLoop loop;
        cabinflow::gateway::ControlGateway gateway(loop, "voice-demo", argv[1],
            static_cast<std::uint16_t>(port), pipeline.registry(), pipeline.runtime(), pipeline.clock(), 300'000);
        gateway.set_data_task_hooks(pipeline.hooks());
        gateway.start();
        const auto startup_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startup_started).count();
        std::cout << "voice_demo_server listening=" << argv[1] << ':' << gateway.bound_port()
                  << " startup_elapsed_ns=" << startup_elapsed_ns
                  << " startup_boundary=model_load_runtime_assembly_tcp_listen"
                  << " models=locked_cpu semantic_quality=separate human_listening=not_verified\n" << std::flush;
        std::exception_ptr shutdown_error;
        std::thread controller([&] {
            try {
                int received = 0;
                const int waited = sigwait(&signals, &received);
                if (waited != 0) throw std::runtime_error("sigwait failed: " + std::to_string(waited));
                pipeline.shutdown();
            } catch (...) {
                shutdown_error = std::current_exception();
            }
            // drain/join 后才让 owner 停 Gateway；关闭前仍允许根 completion 交付最终取消结果。
            loop.queue_in_loop([&] { gateway.stop(); loop.quit(); });
        });
        try {
            loop.loop();
        } catch (...) {
            const auto loop_error = std::current_exception();
            static_cast<void>(pthread_kill(controller.native_handle(), SIGTERM));
            controller.join();
            gateway.stop();
            std::rethrow_exception(loop_error);
        }
        controller.join();
        if (shutdown_error) std::rethrow_exception(shutdown_error);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "voice_demo_server failed: " << error.what() << '\n';
        return 1;
    }
}
