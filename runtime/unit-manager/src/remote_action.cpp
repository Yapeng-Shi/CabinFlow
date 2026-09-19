/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "all.h"
#include <string>
#include "remote_action.h"
#include "pzmq.hpp"
#include "json.hpp"
#include "StackFlowUtil.h"
#include <simdjson.h>

using namespace StackFlows;

// setup操作，通过RPC接口，调用另一个进程的setup功能，完成节点注册和资源准备
// 参数：com_id 是当前 zmq_bus_com 通信会话的端口，json_str 是包含 setup 请求参数的 JSON 字符串
int remote_call(int com_id, const std::string &json_str)
{
    // json数据 string类型转化为 simdjson可操作的类型
    simdjson::ondemand::parser parser;
    simdjson::padded_string json_string(json_str);
    simdjson::ondemand::document doc;
    auto error = parser.iterate(json_string).get(doc);

    // 从 JSON 字符串解析出 request_id、work_id 和 action 字段，进行基本的格式校验
    std::string work_id;
    doc["work_id"].get_string(work_id);
    std::string work_unit = work_id.substr(0, work_id.find("."));
    std::string action;
    doc["action"].get_string(action);

    if (work_id.empty() || action.empty()) {
        throw std::runtime_error("Invalid JSON: missing work_id or action");
    }

    // PUSH-PULL 的 URL : 要传给业务节点
    // 根据当前通信会话的端口号和地址模板格式化出完整的 ZMQ 地址，供后续 RPC 调用使用
    char com_url[256];
    snprintf(com_url, 255, zmq_c_format.c_str(), com_id);
    // 创建一个临时的 pzmq 客户端对象，连接到业务节点的 RPC 服务端，发送 action 和参数数据，并等待响应
    pzmq client(work_unit);
    // RPC 客户端调用：发送 action + set_param序列化后的参数数据，等待服务端响应，并把响应交给回调处理
    return client.call_rpc_action(action, pzmq_data::set_param(com_url, json_str),
                                  [](pzmq *_pzmq, const std::shared_ptr<pzmq_data> &val) {});
}
