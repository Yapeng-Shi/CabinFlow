
/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "zmq_bus.h"

#include "all.h"
#include <stdbool.h>
#include <functional>
#include <cstring>
#include <StackFlowUtil.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#ifdef ENABLE_BSON
#include <bson/bson.h>
#endif

using namespace StackFlows;

zmq_bus_com::zmq_bus_com()
{
    exit_flage      = 1;
    err_count       = 0;
    json_str_flage_ = 0;
}

// TCP-ZMQ-PUSH 的桥接函数，把“当前 TCP 会话”绑定到一个对应的 ZMQ 接收端上，形成 TCP 和 ZMQ 之间的数据桥接
// 入口参数是 ZMQ 地址模板和端口号
// 为当前 zmq_bus_com 对象初始化一个“接收 ZMQ 消息的通道”，并把收到的数据转交给当前对象的 send_data() 处理
void zmq_bus_com::work(const std::string &zmq_url_format, int port)
{
    // 保存当前会话绑定的端口号，后续任务分发和资源管理会用到
    // 初始化：PUSH-PULL 的 url
    // 8000
    _port = port;

    // 标记当前对象处于运行状态
    exit_flage        = 1;
    std::string ports = std::to_string(port);
    std::vector<char> buff(zmq_url_format.length() + ports.length(), 0);

    // 按模板格式化出完整地址，ipc:///tmp/llm/8000.sock
    sprintf((char *)buff.data(), zmq_url_format.c_str(), port);
    // 保存最终生成的 ZMQ 地址，供当前会话使用和清理
    _zmq_url = std::string((char *)buff.data());

    // 创建一个独占的 pzmq 通道对象，并交给 user_chennal_ 管理
    // 这里使用 ZMQ_PULL，表示当前端负责接收来自对端的消息
    // 收到消息后会回调 this->send_data，由子类决定如何把数据继续转发出去
    user_chennal_ = std::make_unique<pzmq>(
        _zmq_url, ZMQ_PULL,
        [this](pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data) { this->send_data(data->string()); });
}

void zmq_bus_com::stop()
{
    exit_flage = 0;
    user_chennal_.reset();
}

// 把当前会话收到的一条业务数据交给系统任务分发器处理
void zmq_bus_com::on_data(const std::string &data)
{
    // TCP传输的数据
    std::cout << "on_data:" << data << std::endl;

    // 对任务分发
    unit_action_match(_port, data);
}

void zmq_bus_com::send_data(const std::string &data)
{
}

zmq_bus_com::~zmq_bus_com()
{
    if (exit_flage) {
        stop();
    }
}

// 将推理任务 pub 给业务节点，供业务节点订阅处理
// PUB给业务节点 -> 业务节点sub -> 业务节点推理 -> PUSH发送给zmq_bus -> PULL接收数据 -> send TCP转发给客户端
int zmq_bus_publisher_push(const std::string &work_id, const std::string &json_str)
{
    ALOGW("zmq_bus_publisher_push json_str:%s", json_str.c_str());

    if (work_id.empty()) {
        ALOGW("work_id is empty");
        return -1;
    }
    unit_data *unit_p = NULL;
    // 通过 work_id 获取对应的 unit_data 对象，包含了业务节点的通信信息（如
    // inference_url），然后通过该对象发送消息到业务节点
    SAFE_READING(unit_p, unit_data *, work_id);
    if (unit_p) unit_p->send_msg(json_str);
    ALOGW("zmq_bus_publisher_push work_id:%s", work_id.c_str());

    else
    {
        ALOGW("zmq_bus_publisher_push failed, not have work_id:%s", work_id.c_str());
        return -1;
    }
    return 0;
}

void *usr_context;

// 任务分发异常情况（客户端数据异常/或者我们自身处理异常），直接把异常信息发送给客户端
// PUSH发送给zmq_bus -> PULL接收数据 ->send TCP转发给客户端
void zmq_com_send(int com_id, const std::string &out_str)
{
    char zmq_push_url[128];
    sprintf(zmq_push_url, zmq_c_format.c_str(), com_id);
    pzmq _zmq(zmq_push_url, ZMQ_PUSH);
    std::string out = out_str + "\n";
    _zmq.send_data(out);
}

// 对收到的 JSON 字符串进行处理，去掉末尾的换行符（如果有），然后交给外部回调函数 out_fun 处理
void zmq_bus_com::select_json_str(const std::string &json_src, std::function<void(const std::string &)> out_fun)
{
    std::string test_json = json_src;

    if (!test_json.empty() && test_json.back() == '\n') {
        test_json.pop_back();
    }
    // 完整的 json 数据，string类型
    out_fun(test_json);
}