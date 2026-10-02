#include <cabinflow/observability/logger.hpp>

#include <chrono>
#include <iostream>
#include <mutex>

namespace cabinflow::observability {
namespace {

std::mutex stdout_mutex;

}  // namespace

void ConsoleLogger::log(const Event& event) {
    std::lock_guard<std::mutex> lock(stdout_mutex);
    // 墙上时间用于跨进程排查；消息 TTL 仍只使用单调时钟，不混用两个时间域。
    const auto time_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout << "time_unix_ms=" << time_unix_ms
              << " event=" << event.name << " node=" << event.component
              << " trace_id=" << event.trace_id
              << " session_id=" << event.session_id
              << " work_id=" << event.work_id
              << " message_id=" << event.message_id
              << " status=" << event.status
              << " detail=" << event.detail << '\n';
}

}  // namespace cabinflow::observability
