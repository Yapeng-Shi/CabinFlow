#include <iostream>
#include "pzmq.hpp"
#include "pzmq_data.h"
#include <string>
#include <unistd.h>  // for sleep()

using namespace StackFlows;

// RPC 方法实现：fun1_
// 解析输入消息，打印参数，并返回一个新的参数组合
std::string fun1_(pzmq* self, const std::shared_ptr<pzmq_data>& msg)
{
    std::string raw_msg = msg->string();
    // 打印客户端消息内容（十六进制格式）
    std::cout << "Raw data (hex): ";
    for (char c : raw_msg) {
        printf("%02X ", static_cast<unsigned char>(c));
    }
    std::cout << std::endl;

    // 拆包
    std::string param0 = msg->get_param(0);
    std::string param1 = msg->get_param(1);

    std::cout << "fun1 received: param0=" << param0 << ", param1=" << param1 << std::endl;
    // 返回 0x5hellosorbai（协议格式：1 字节 param0 长度 + param0 + param1）
    return pzmq_data::set_param("hello", "sorbai");
}

int main(int argc, char* argv[])
{
    pzmq _rpc("test");
    _rpc.register_rpc_action("fun1", fun1_);

    while (1) {
        sleep(1);
    }
    return 0;
}