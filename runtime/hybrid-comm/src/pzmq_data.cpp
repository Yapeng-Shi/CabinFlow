/**
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "pzmq_data.h"

namespace StackFlows {

// 构造函数：初始化 ZeroMQ 消息对象，后续可用于接收/发送或承载数据
pzmq_data::pzmq_data()
{
    zmq_msg_init(&msg);
}

// 析构函数：释放 ZeroMQ 消息对象内部资源，避免内存泄漏
pzmq_data::~pzmq_data()
{
    zmq_msg_close(&msg);
}

// 将消息内容拷贝为 shared_ptr<string> 返回
// 这里会发生一次数据拷贝，返回对象可在外部共享
std::shared_ptr<std::string> pzmq_data::get_string()
{
    auto len = zmq_msg_size(&msg);
    return std::make_shared<std::string>((const char*)zmq_msg_data(&msg), len);
}

// 将消息内容拷贝为 string 返回
// 与 get_string() 类似，但返回值语义不同（非共享）
std::string pzmq_data::string()
{
    auto len = zmq_msg_size(&msg);
    return std::string((const char*)zmq_msg_data(&msg), len);
}

// 获取消息体原始数据指针（指向 zmq_msg_t 内部缓冲）
// 注意：指针生命周期受 msg 生命周期约束
void* pzmq_data::data()
{
    return zmq_msg_data(&msg);
}

// 获取消息体字节长度
size_t pzmq_data::size()
{
    return zmq_msg_size(&msg);
}

// 获取底层 zmq_msg_t 指针，便于直接调用 zmq API
zmq_msg_t* pzmq_data::get()
{
    return &msg;
}

// 反序列化接口
// 按约定格式解析参数：
// [0]      : param0 长度（1 字节）
// [1..N]   : param0 数据
// [N+1..]  : param1 数据
// index 偶数返回 param0，奇数返回 param1
// idata 非空时优先解析 idata，否则解析当前 msg
std::string pzmq_data::get_param(int index, const std::string& idata)
{
    const char* data = nullptr;
    int size         = 0;

    if (idata.length() > 0) {
        data = idata.c_str();
        size = static_cast<int>(idata.length());
    } else {
        data = static_cast<const char*>(zmq_msg_data(&msg));
        size = static_cast<int>(zmq_msg_size(&msg));
    }

    if ((index % 2) == 0) {
        // 如果 index 是偶数（取 param0） bilibili
        // index = 0，
        // char* data =  0x08bilibilisorbai
        // data + 1 -> bilibilisorbai
        // static_cast<size_t>(8)
        // bilibili
        // 返回 param0
        return std::string(data + 1, static_cast<size_t>(data[0]));
    } else {
        // 返回 param1
        return std::string(data + data[0] + 1, static_cast<size_t>(size - data[0] - 1));
    }
}

// 序列化接口
// 按协议打包两个参数：[1 字节 param0 长度][param0][param1]
std::string pzmq_data::set_param(std::string param0, std::string param1)
{
    std::string data = " " + param0 + param1;
    data[0]          = static_cast<char>(param0.length());
    return data;
}

}  // namespace StackFlows