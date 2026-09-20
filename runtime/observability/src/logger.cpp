#include <cabinflow/observability/logger.hpp>

#include <iostream>
#include <mutex>

namespace cabinflow::observability {
namespace {

std::mutex stdout_mutex;

}  // namespace

void ConsoleLogger::log(const Event& event) {
    std::lock_guard<std::mutex> lock(stdout_mutex);
    std::cout << "event=" << event.name << " component=" << event.component
              << " trace_id=" << event.trace_id
              << " session_id=" << event.session_id
              << " work_id=" << event.work_id
              << " message_id=" << event.message_id
              << " detail=" << event.detail << '\n';
}

}  // namespace cabinflow::observability
