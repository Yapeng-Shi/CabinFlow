// 删除原有hv头文件，添加：
#pragma once

#include "network/TcpServer.h"
#include "network/EventLoop.h"
#include <any>
#include <atomic>

#include "zmq_bus.h"
#include "network/TcpConnection.h"

// 管理 TCP 长连接的会话类，负责上下文管理和消息转发，作为 TCP 连接和 ZMQ 发布通道之间的桥梁
class TcpSession : public zmq_bus_com {
public:
    explicit TcpSession(const network::TcpConnectionPtr &conn) : conn_(conn)
    {
    }

    // 对外部用户通信，负责将 ZMQ 消息转发到 TCP 连接中，作为 TCP 消息发送给客户端
    // 业务节点推理完数据 -> PUSH -> PELL接收 -> send_data 发送客户端
    void send_data(const std::string &data) override
    {
        printf("zmq_bus_com::send_data : send:%s\n", data.c_str());
        // 将字符串数据封装到 Buffer 中，并通过 TCP 连接发送出去
        network::Buffer *buf = new network::Buffer;
        buf->append(data.c_str(), data.size());
        conn_->send(buf);
    }

    // 当前会话对应的 TCP 连接对象
    network::TcpConnectionPtr conn_;
};