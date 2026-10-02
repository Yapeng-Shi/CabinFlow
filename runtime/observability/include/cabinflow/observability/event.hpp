#pragma once

#include <string>

namespace cabinflow::observability {

struct Event {
    std::string component;
    std::string name;
    std::string trace_id;
    std::string session_id;
    std::string work_id;
    std::string message_id;
    std::string detail;
    std::string status{};
};

}  // namespace cabinflow::observability
