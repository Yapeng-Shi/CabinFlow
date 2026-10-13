#pragma once

#include <string_view>
#include <cockpit_text.pb.h>

namespace cabinflow::agent {
// 原分级器的规则，不包含模型调用或故障回退。
v1::AnswerRoute classify_answer_route(std::string_view query);
}
