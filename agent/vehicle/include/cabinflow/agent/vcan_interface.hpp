#pragma once

#include <string>

namespace cabinflow::agent::vehicle_can {

// 从内核 rtnetlink 读取接口 kind，名字为 vcan0 或 ARPHRD_CAN 都不足以排除物理 CAN。
[[nodiscard]] bool is_vcan_interface(unsigned interface_index, std::string& detail);

}  // namespace cabinflow::agent::vehicle_can
