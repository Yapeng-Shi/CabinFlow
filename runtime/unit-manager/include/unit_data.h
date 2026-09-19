#pragma once

#include "pzmq.hpp"
#include <vector>

using namespace StackFlows;

// 管理一个TASK中所有元数据信息
// 单个业务单元（unit/task）的运行时元数据 + 发送通道封装
class unit_data {
private:
    // 外部用户推理问题，通过该 pub 通道发送给业务节点 sub
    std::unique_ptr<pzmq> user_inference_chennal_;

public:
    std::string work_id;        // unit 单元标识，全局系统-唯一标识
    std::string output_url;     // pub，节点内部pub url，与其他节点进行pub - sub交互 - 其他节点
    std::string inference_url;  // pub 对外部用户，用户pub 的url，业务节点通过这个url订阅用户的推理请求
    int port_;                  // 端口号，作为 unit 的唯一标识之一，格式为 "unit.port"

    unit_data();
    void init_zmq(const std::string &url);
    void send_msg(const std::string &json_str);
    ~unit_data();
};
