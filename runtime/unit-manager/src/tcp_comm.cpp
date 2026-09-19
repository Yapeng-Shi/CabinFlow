
#include <unordered_map>
#include <unistd.h>
#include <chrono>
#include <any>
#include <cstring>
#include <ctime>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "all.h"
#include "session.h"
#include "zmq_bus.h"
#include <boost/any.hpp>
#include "json.hpp"
#include <variant>

std::atomic<int> counter_port(8000);         // 原子端口计数器，负责给每个新会话分配唯一端口，初始从 8000 开始
network::EventLoop loop;                     // 网络事件循环对象，负责驱动 TCP 连接、读写和回调处理
std::unique_ptr<network::TcpServer> server;  // TCP 服务器对象，独占管理，负责监听客户端连接并分发消息
std::mutex context_mutex;                    // 上下文互斥锁，用于保护共享状态的并发访问

// TCP 连接回调函数接口，当客户端-服务端连接建立或断开时被调用，负责创建或销毁 TcpSession 对象，并管理其生命周期
void onConnection(const network::TcpConnectionPtr &conn)
{
    if (conn->connected()) {
        // 会话窗口：创建 TcpSession 对象，绑定到 TCP 连接上下文中，处理后续消息-类似微信聊天窗口
        std::shared_ptr<TcpSession> session = std::make_shared<TcpSession>(conn);
        conn->setContext(session);
        // 为当前 TCP 会话创建一个对应的 ZMQ 接收端，形成 TCP 和 ZMQ 之间的数据桥接
        session->work(zmq_s_format, counter_port.fetch_add(1));

        // 端口号自增分配，确保每个会话对应一个唯一的 ZMQ 通道，避免冲突
        if (counter_port > 65535) {
            counter_port.store(8000);
        }
    } else {
        try {
            // 连接断开时，从上下文中获取对应的 TcpSession 对象，并调用其 stop 方法清理资源
            auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());
            session->stop();
        } catch (const std::bad_any_cast &e) {
            std::cerr << "Bad any_cast: " << e.what() << std::endl;
        }
    }
}

// void onConnection(const TcpConnectionPtr &conn)
// {
//     if (conn->connected())
//     {
//         Context context;
//         conn->setContext(context);
//     }
//     else
//     {
//         const Context &context = boost::any_cast<Context>(conn->getContext());
//         LOG_INFO << "payload bytes " << context.bytes;
//         conn->getLoop()->quit();
//     }
// }

// TCP 消息回调函数接口，当连接接收到消息时被调用，负责解析消息内容，并调用 TcpSession 的 select_json_str
// 方法处理业务逻辑
void onMessage(const network::TcpConnectionPtr &conn, network::Buffer *buf)
{
    // 解析消息内容，获取 JSON 字符串
    std::string msg(buf->retrieveAllAsString());

    try {
        // 获取当前 TCP 会话对应的 TcpSession 对象，上下文会话窗口
        auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());
        // 调用 TcpSession 的 select_json_str 方法处理业务逻辑，传入消息内容和一个回调函数
        session->select_json_str(msg, std::bind(&TcpSession::on_data, session, std::placeholders::_1));
    } catch (const boost::bad_any_cast &e) {
        std::cerr << "Type cast error: " << e.what() << std::endl;
    }
}

// 启动并运行 unit-manager 的 TCP 接入层，让外部客户端请求进入系统，并交给后续的 ZMQ/RPC 逻辑处理
void tcp_work()
{
    int listenport = 0;  // 默认端口号，实际使用时从配置文件中读取
    // 主线程，Reactor 线程，负责监听 TCP 连接，分发消息
    // 获取系统 sql 键值数据库中的参数配置
    SAFE_READING(listenport, int, "config_tcp_server");

    // 根据端口号 listenport 创建一个监听地址对象，用于后面给 TcpServer 绑定并监听
    network::InetAddress listenAddr(listenport);

    // 创建 TcpServer 对象，绑定事件循环 loop 和监听地址 listenAddr，命名为 "ZMQBridge"
    // TcpServer 对象只需要被 ZMQBridgeServer 一个类管理，不需要共享，使用 unique_ptr 来管理其生命周期
    server = std::make_unique<network::TcpServer>(&loop, listenAddr, "ZMQBridge");

    // 设置连接回调函数 onConnection 和消息回调函数 onMessage，分别处理连接建立/断开和消息接收事件
    server->setConnectionCallback(onConnection);
    server->setMessageCallback(onMessage);

    // IO 线程，负责处理消息，分发给业务线程池
    // I/O 线程池中的线程数量，默认 0 线程表示所有 IO 在主线程处理，实际使用时可以根据负载调整线程数
    server->setThreadNum(2);

    // 启动 TCP 服务器，开始监听客户端连接，并进入事件循环，等待和处理事件
    server->start();
    // 事件循环 loop 会持续运行，直到调用 loop.quit() 停止，期间会处理所有 TCP 连接和消息事件
    loop.loop();
}

void tcp_stop_work()
{
    loop.quit();
    server.reset();
}
