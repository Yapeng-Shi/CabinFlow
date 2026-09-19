/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "pzmq.hpp"
#include <vector>
#include "unit_data.h"

using namespace StackFlows;

int zmq_bus_publisher_push(const std::string &work_id, const std::string &json_str);
void zmq_com_send(int com_id, const std::string &out_str);

// 本质上是一个 TCP 会话与 ZMQ 通道之间的“桥接基类”，负责把通信链路的公共能力封装起来
// 每当有一个新的 TCP 连接建立，就会 new 一个 TcpSession（继承zmq_bus_com）
// 作为该连接的上下文对象，负责管理该连接的 ZMQ 通道和消息转发
class zmq_bus_com {
protected:
    std::string _zmq_url;   // 当前实例绑定的 ZMQ 地址，用于创建和清理通信通道
    int exit_flage;         // 退出标志，控制当前通信循环或资源是否需要停止
    int err_count;          // 错误计数器，记录通信过程中的异常次数，便于调试和限流处理
    int _port;              // 当前会话关联的端口号，用于映射对应的业务单元或任务
    std::string json_str_;  // 暂存当前收到的 JSON 字符串，通常用于解析前的缓冲
    int json_str_flage_;    // JSON 处理状态标志，表示当前字符串是否已处理或已进入某个阶段

public:
    std::unique_ptr<pzmq> user_chennal_;  // 当前会话对应的 ZMQ 通道对象，负责收发消息

    zmq_bus_com();                                           // 初始化基础状态
    void work(const std::string &zmq_url_format, int port);  // 按格式创建并启动 ZMQ 通道
    void stop();                                             // 停止当前会话的 ZMQ 通道并释放资源
    void select_json_str(const std::string &json_src,
                         std::function<void(const std::string &)> out_fun);  // 处理收到的 JSON 字符串并交给外部回调
    virtual void on_data(const std::string &data);    // 接收到上游数据后的默认处理入口，通常负责任务分发
    virtual void send_data(const std::string &data);  // 将下游数据发送出去，子类通常会重写这个接口
    ~zmq_bus_com();                                   // 析构时自动清理资源
};