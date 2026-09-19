/**
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "zmq.h"
#include <memory>
#include <string>

namespace StackFlows {

// ZMQ 消息的封装类，主要用于简化 ZMQ消息的创建、访问、生命周期管理；提供序列化/反序列化接口
// 把 ZeroMQ 的底层消息对象 zmq_msg_t 做成一个安全、易用的 C++ 包装类
class pzmq_data {
private:
    zmq_msg_t msg;  // ZeroMQ 原始消息对象（生命周期由本类管理）

public:
    pzmq_data();
    ~pzmq_data();

    // 把消息内容拷贝成字符串，方便上层业务直接处理 JSON 或文本
    std::shared_ptr<std::string> get_string();
    std::string string();

    // 返回消息数据指针（指向 msg 内部缓冲区）
    // 注意：指针有效期受 msg 生命周期影响
    void *data();

    // 返回当前消息字节数
    size_t size();

    // 返回底层 zmq_msg_t，供 zmq_* API 直接使用
    zmq_msg_t *get();

    // 解析参数协议：
    // data[0] 为 param0 长度（1 字节）
    // 紧随其后是 param0 与 param1 的拼接数据
    // index 为偶数返回 param0，为奇数返回 param1
    // 当 idata 非空时优先解析 idata，否则解析当前 msg 内容
    std::string get_param(int index, const std::string &idata = "");

    // 按约定协议打包两个参数：
    // [1字节 param0 长度][param0][param1]
    static std::string set_param(std::string param0, std::string param1);
};

}  // namespace StackFlows