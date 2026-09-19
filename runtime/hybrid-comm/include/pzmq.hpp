/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <libzmq/zmq.h>
#include <memory>
#include <functional>
#include <thread>
#include <iostream>
#include <string>
#include <atomic>
#include <unordered_map>
#include <unistd.h>
#include <mutex>
#include <vector>
#include "pzmq_data.h"

#define ZMQ_RPC_FUN  (ZMQ_REP | 0x80)
#define ZMQ_RPC_CALL (ZMQ_REQ | 0x80)

namespace StackFlows {

// “ZMQ 通信基础设施层”，上层像 zmq_bus_com、unit_data 这些类都是基于它来做具体业务桥接的
/*
1、统一支持多种通信模式
pzmq 既能做发布订阅，也能做 PUSH/PULL，还能做 RPC 服务端和客户端。并在creat()里按模式分发到
creat_pub、creat_push、creat_pull、creat_rep、creat_req 等实现。
2、管理 ZeroMQ 生命周期
它内部持有 zmq_ctx_ 和zmq_socket_，负责创建、连接、绑定和销毁，以及close_zmq()和析构函数。
3、提供回调式收消息机制
对接收型模式，它会起后台线程跑zmq_event_loop()，收到消息后调用回调函数，把消息交给上层处理
4、支持 RPC 动作注册与调用
它维护了 action -> 回调 的映射表 zmq_fun_，允许注册、注销、查询 action，也支持 call_rpc_action()这种请求-响应式调用
*/
class pzmq {
public:
    // RPC 回调：入参为当前 pzmq 对象和消息体，返回字符串作为 RPC 响应
    using rpc_callback_fun = std::function<std::string(pzmq *, const std::shared_ptr<pzmq_data> &)>;
    // 普通消息回调：入参为当前 pzmq 对象和消息体，无返回值
    using msg_callback_fun = std::function<void(pzmq *, const std::shared_ptr<pzmq_data> &)>;

public:
    const int rpc_url_head_length = 6;                  // "ipc://" 前缀长度（用于从 URL 中截取本地 socket 文件路径）
    std::string rpc_url_head_     = "ipc:///tmp/rpc.";  // 默认 RPC 地址前缀；传入完整 URL 时会被清空
    void *zmq_ctx_;                                     // ZeroMQ 上下文句柄
    void *zmq_socket_;                                  // ZeroMQ socket 句柄

    // RPC action 映射表，key 是 action 名称，value 是对应的处理函数（回调）
    std::unordered_map<std::string, rpc_callback_fun> zmq_fun_;

    // 互斥锁-》读写锁-》自旋锁（适用于高发短处理）-》无锁机制（本质就会用到原子操作）
    std::mutex zmq_fun_mtx_;  // 保护 zmq_fun_ 的互斥锁（多线程访问）

    std::atomic<bool> flage_;                  // 事件循环退出标志：false 运行，true 停止
    std::unique_ptr<std::thread> zmq_thread_;  // 后台收发线程（SUB/PULL/RPC_FUN 等模式会启用）
    int mode_;                                 // 当前通信模式（ZMQ_PUB / ZMQ_SUB / ZMQ_RPC_FUN 等）
    std::string rpc_server_;                   // RPC 服务名或服务地址（懒创建场景使用）
    std::string zmq_url_;                      // 当前实际使用的 ZMQ URL（用于清理资源）
    int timeout_;                              // 发送/接收超时（毫秒）

    bool is_bind()
    {
        if ((mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN))
            return true;
        else
            return false;
    }

public:
    // 构造方式1：RPC 懒创建模式
    // 传入 server 名称或完整地址，只记录参数，不立即创建 zmq ctx/socket。
    // 适合后续通过 register_rpc_action 或 call_rpc_action 再触发实际建链。
    pzmq(const std::string &server)
        : zmq_ctx_(NULL), zmq_socket_(NULL), rpc_server_(server), flage_(true), timeout_(3000)
    {
        // 如果传入的是完整 URL（包含 ://），则不再拼接默认 ipc 前缀
        if (server.find("://") != std::string::npos) {
            rpc_url_head_.clear();
        }
    }

    // 构造方式2：指定通信模式创建
    // 传入 url + mode（可选 raw_call），多数模式会在构造阶段立即 creat(url, raw_call）。
    // 仅 mode == ZMQ_RPC_FUN 时不在这里创建（服务端由注册 action 时懒创建）。
    pzmq(const std::string &url, int mode, const msg_callback_fun &raw_call = nullptr)
        : zmq_ctx_(NULL), zmq_socket_(NULL), mode_(mode), flage_(true), timeout_(3000)
    {
        // 非 ipc 地址时，清空默认前缀，避免地址拼接错误
        if ((url[0] != 'i') && (url[1] != 'p')) {
            rpc_url_head_.clear();
        }
        // 非 RPC_FUN 模式：构造时立即创建底层资源
        if (mode_ != ZMQ_RPC_FUN) creat(url, raw_call);
    }

    // 内建 RPC action：查询当前已注册的 action 列表，返回 JSON 格式字符串
    // action_list 格式示例：{"actions":["fun1","fun2","list_action"]}
    std::string _rpc_list_action(pzmq *self, const std::shared_ptr<pzmq_data> &_None)
    {
        std::string action_list;
        action_list.reserve(128);
        action_list = "{\"actions\":[";
        for (auto i = zmq_fun_.begin();;) {
            action_list += "\"";
            action_list += i->first;
            action_list += "\"";
            if (++i == zmq_fun_.end()) {
                action_list += "]}";
                break;
            } else {
                action_list += ",";
            }
        }
        return action_list;
    }

    // 在 RPC 服务端注册（或更新）一个 action 对应的处理函数，并在首次注册时把整个 RPC 服务端真正启动起来
    int register_rpc_action(const std::string &action, const rpc_callback_fun &raw_call)
    {
        // 返回值约定：0 成功；非 0 通常来自首次创建服务端时的底层错误
        int ret = 0;

        // 保护 zmq_fun_：避免与事件线程读取/其他线程注册发生并发竞态
        std::unique_lock<std::mutex> lock(zmq_fun_mtx_);

        // 若 action 已存在，则执行“热更新”逻辑：直接替换回调并返回
        // 这样可在运行中覆盖旧实现，无需先 unregister 再 register
        if (zmq_fun_.find(action) != zmq_fun_.end()) {
            zmq_fun_[action] = raw_call;
            return ret;
        }

        // 首次注册（映射表为空）时，执行 RPC 服务端懒创建：
        // 1) 组装最终监听地址；
        // 2) 设置当前模式为 RPC_FUN（服务端）；
        // 3) 注入内建 action：list_action；
        // 4) 调用 creat(url) 完成 bind 与事件线程启动。
        if (zmq_fun_.empty()) {
            std::string url = rpc_url_head_ + rpc_server_;
            mode_           = ZMQ_RPC_FUN;
            zmq_fun_["list_action"] =
                std::bind(&pzmq::_rpc_list_action, this, std::placeholders::_1, std::placeholders::_2);
            ret = creat(url);
        }

        // 写入用户注册的 action 回调（首次注册或后续新增都会走到这里）
        zmq_fun_[action] = raw_call;

        // 返回创建结果：若已创建过服务端，ret 保持 0
        return ret;
    }

    // 注销一个 RPC action 回调
    void unregister_rpc_action(const std::string &action)
    {
        std::unique_lock<std::mutex> lock(zmq_fun_mtx_);
        if (zmq_fun_.find(action) != zmq_fun_.end()) {
            zmq_fun_.erase(action);
        }
    }

    // pzmq 里的 RPC 客户端入口，负责把一次 action 调用发出去，并把服务端返回结果拿回来
    int call_rpc_action(const std::string &action, const std::string &data, const msg_callback_fun &raw_call)
    {
        int ret = 0;  // 返回码：0 代表成功，非 0 代表创建/通信阶段出现错误

        // 用于接收服务端响应消息
        std::shared_ptr<pzmq_data> msg_ptr = std::make_shared<pzmq_data>();

        try {
            // 懒创建客户端 socket：首次调用时才真正创建 REQ 连接
            if (NULL == zmq_socket_) {
                // 未配置服务端名称，无法拼接目标地址，直接失败
                if (rpc_server_.empty()) return -1;

                // 拼接 RPC 服务地址并切换到 RPC 客户端模式
                std::string url = rpc_url_head_ + rpc_server_;
                mode_           = ZMQ_RPC_CALL;

                // 创建并连接 socket（内部会走 creat_req）
                ret = creat(url);
                if (ret) {
                    // 统一走 catch 分支处理错误码
                    throw ret;
                }
            }

            // 发送请求（两帧）：
            // 第1帧：action 名称，使用 ZMQ_SNDMORE 表示后续还有帧
            // 第2帧：业务数据（请求体），0 标志​：表示当前帧是消息的结束
            {
                zmq_send(zmq_socket_, action.c_str(), action.length(), ZMQ_SNDMORE);
                zmq_send(zmq_socket_, data.c_str(), data.length(), 0);
            }

            // 接收服务端响应（单帧），0 标志：表示当前帧是消息的结束
            {
                zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0);
            }

            // 将响应交给调用方提供的回调处理
            raw_call(this, msg_ptr);

        } catch (int e) {
            // 记录错误码，函数末尾统一返回
            ret = e;
        }

        // 主动释放消息对象（可省略，离开作用域也会自动释放）
        msg_ptr.reset();

        // 当前实现每次调用后都会关闭 socket，下次调用再懒创建
        // 这种方式简单，但会增加频繁建连开销
        close_zmq();

        return ret;
    }

    // 创建 pzmq 底层通信资源，并按当前 mode_ 分发到对应的具体建链逻辑
    int creat(const std::string &url, const msg_callback_fun &raw_call = nullptr)
    {
        zmq_url_ = url;

        // 创建 ZeroMQ 上下文和 socket，直到成功为止（通常不会失败，除非系统资源极度紧张）
        do {
            zmq_ctx_ = zmq_ctx_new();
        } while (zmq_ctx_ == NULL);
        do {
            zmq_socket_ = zmq_socket(zmq_ctx_, mode_ & 0x3f);
        } while (zmq_socket_ == NULL);

        switch (mode_) {
            case ZMQ_PUB: {
                return creat_pub(url);
            } break;
            case ZMQ_SUB: {
                return subscriber_url(url, raw_call);
            } break;
            case ZMQ_PUSH: {
                return creat_push(url);
            } break;
            case ZMQ_PULL: {
                return creat_pull(url, raw_call);
            } break;
            case ZMQ_RPC_FUN: {
                return creat_rep(url, raw_call);
            } break;
            case ZMQ_RPC_CALL: {
                return creat_req(url);
            } break;
            default:
                break;
        }
        return 0;
    }

    // 发送数据（适用于 PUSH 模式和 RPC 客户端模式）
    int send_data(const std::string &raw)
    {
        return zmq_send(zmq_socket_, raw.c_str(), raw.length(), 0);
    }

    inline int creat_pub(const std::string &url)
    {
        return zmq_bind(zmq_socket_, url.c_str());
    }
    inline int creat_push(const std::string &url)
    {
        // PUSH 端通常作为发送方连接到远端（例如 PULL 服务端）。
        // 当网络抖动或对端重启时，ZeroMQ 会自动重连；下面两个参数控制重连节奏。

        // 初始重连间隔（毫秒）。
        // 含义：连接断开后，第一次尝试重连前等待 100ms。
        int reconnect_interval = 100;
        zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_interval, sizeof(reconnect_interval));

        // 最大重连间隔（毫秒）。
        // ZeroMQ 可能采用退避策略逐步拉长重连等待时间，但不会超过该上限。
        // 这里设置为 1000ms，也就是 1 秒。
        int max_reconnect_interval = 1000;  // 1 second
        zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_reconnect_interval, sizeof(max_reconnect_interval));

        // 发送超时（毫秒）。
        // 当发送队列拥塞、对端暂不可达等导致发送无法及时完成时，
        // 到达 timeout_ 后 zmq_send 会超时返回，而不是无限阻塞调用线程。
        zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));

        // 发起到目标地址的连接（例如 tcp://host:port 或 ipc:///path）。
        // 返回值约定：
        // 0   : 调用成功（连接流程已交给 ZeroMQ 管理，后续可自动重连）
        // -1  : 调用失败（可结合 errno / zmq_strerror(errno) 排查原因）
        return zmq_connect(zmq_socket_, url.c_str());
    }
    inline int creat_pull(const std::string &url, const msg_callback_fun &raw_call)
    {
        // PULL 端作为接收方，通常由本端绑定地址，等待 PUSH 端连接并发送消息。
        // 如果 bind 失败，ret 为 -1；成功为 0（可结合 errno 判断失败原因）。
        int ret = zmq_bind(zmq_socket_, url.c_str());

        // 将事件循环退出标志置为 false，表示接收线程可以开始运行。
        // 事件循环中通过 while (!flage_.load()) 持续收消息。
        flage_ = false;

        // 启动后台线程执行统一事件循环：
        // 1) 在循环里执行 zmq_poll/zmq_msg_recv
        // 2) 收到消息后回调 raw_call(this, msg_ptr)
        // 这样主线程无需阻塞在接收操作上。
        zmq_thread_ = std::make_unique<std::thread>(std::bind(&pzmq::zmq_event_loop, this, raw_call));

        // 返回 bind 的结果，交由上层决定是否继续运行或做错误处理。
        return ret;
    }

    inline int subscriber_url(const std::string &url, const msg_callback_fun &raw_call)
    {
        // 断线后首次重连间隔：100ms
        int reconnect_interval = 100;
        zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_interval, sizeof(reconnect_interval));

        // 重连间隔上限：1000ms（指数退避不会超过这个值）
        int max_reconnect_interval = 1000;  // 1 second
        zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_reconnect_interval, sizeof(max_reconnect_interval));

        // 作为 SUB 客户端连接到发布端
        int ret = zmq_connect(zmq_socket_, url.c_str());

        // 订阅所有主题（空过滤器）
        zmq_setsockopt(zmq_socket_, ZMQ_SUBSCRIBE, "", 0);

        // 启动后台事件循环线程处理接收消息
        flage_      = false;
        zmq_thread_ = std::make_unique<std::thread>(std::bind(&pzmq::zmq_event_loop, this, raw_call));
        return ret;
    }

    // RPC 服务端创建
    inline int creat_rep(const std::string &url, const msg_callback_fun &raw_call)
    {
        // REP（Reply）端作为 RPC 服务端，通常在本地地址上 bind，等待远端 REQ 客户端连接并发起请求。
        // 返回值语义：0 成功，-1 失败（失败时可结合 errno/zmq_strerror(errno) 排查）。
        int ret = zmq_bind(zmq_socket_, url.c_str());

        // 启动事件循环前，将退出标志置为 false：
        // while (!flage_.load()) 将持续接收并处理请求。
        flage_ = false;

        // 创建后台线程运行统一事件循环，避免主线程阻塞在 recv 上。
        // 在 RPC_FUN 模式下，事件循环会：
        // 1) 先收 action 名称
        // 2) 再收请求数据
        // 3) 查找并执行已注册回调
        // 4) 将回调返回字符串发回客户端
        zmq_thread_ = std::make_unique<std::thread>(std::bind(&pzmq::zmq_event_loop, this, raw_call));

        // 向上层返回 bind 结果，由调用方决定是否继续运行或做失败处理。
        return ret;
    }

    // RPC 客户端创建
    inline int creat_req(const std::string &url)
    {
        // 当使用默认 ipc 前缀时（例如 ipc:///tmp/rpc.xxx），
        // 这里先做一次本地 socket 文件存在性检查，避免无意义 connect。
        // rpc_url_head_ 为空通常表示用户传入了完整 URL（如 tcp://...），此时跳过该检查。
        if (!rpc_url_head_.empty()) {
            // 去掉 "ipc://" 前缀，得到实际文件路径（如 /tmp/rpc.test）
            std::string socket_file = url.substr(rpc_url_head_length);

            // 目标文件不存在时直接返回 -1，表示服务端尚未就绪或地址无效。
            // 注意：这里只是快速失败检查，不代表后续 connect 一定成功。
            if (access(socket_file.c_str(), F_OK) != 0) {
                return -1;
            }
        }

        // 设置发送超时（毫秒）：
        // 如果请求发送阶段因对端不可达/队列阻塞等无法及时完成，
        // zmq_send 会在 timeout_ 到期后返回，而不是无限阻塞。
        zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));

        // 设置接收超时（毫秒）：
        // REQ 发出请求后等待服务端响应，超过 timeout_ 仍未收到则超时返回，
        // 便于上层做重试、降级或错误处理。
        zmq_setsockopt(zmq_socket_, ZMQ_RCVTIMEO, &timeout_, sizeof(timeout_));

        // 作为 REQ 客户端连接目标地址。
        // 返回值：0 表示调用成功，-1 表示失败（可结合 errno/zmq_strerror(errno) 排查）。
        return zmq_connect(zmq_socket_, url.c_str());
    }

    // 统一事件循环：适用于 SUB / PULL / RPC_FUN 三种接收型模式
    void zmq_event_loop(const msg_callback_fun &raw_call)
    {
        // 给线程命名，便于调试时在 top/htop/gdb 中识别
        pthread_setname_np(pthread_self(), "zmq_event_loop");

        int ret;

        // zmq_poll 需要的事件数组，PULL 模式需要监控可读事件，SUB/RPC_FUN 模式直接 recv 即可，无需 poll
        zmq_pollitem_t items[1];

        // 仅 PULL 模式需要显式 poll 等待可读事件。
        // SUB / RPC_FUN 场景直接使用阻塞 recv 即可。
        if (mode_ == ZMQ_PULL) {
            items[0].socket  = zmq_socket_;
            items[0].fd      = 0;
            items[0].events  = ZMQ_POLLIN;
            items[0].revents = 0;
        };

        // flage_ == false 表示继续运行；析构时会置 true 通知线程退出
        while (!flage_.load()) {
            // 每轮循环创建一个消息对象承载本次接收的数据
            std::shared_ptr<pzmq_data> msg_ptr = std::make_shared<pzmq_data>();

            if (mode_ == ZMQ_PULL) {
                // PULL 模式先 poll：无消息则阻塞等待，避免无意义 recv
                ret = zmq_poll(items, 1, -1);

                // poll 出错：关闭当前 socket 并进入下一轮
                // （这里的恢复策略较粗糙，实际生产中可考虑重建 socket 并记录日志）
                if (ret == -1) {
                    zmq_close(zmq_socket_);
                    continue;
                }

                // 非可读事件直接跳过（理论上很少发生）
                if (!(items[0].revents & ZMQ_POLLIN)) {
                    continue;
                }
            }

            // SUB / PULL / RPC_FUN 模式直接 recv：如果无消息或发生错误，recv 会返回 <=0，
            // 此时丢弃本轮并继续等待下一条消息 接收一帧消息；<=0 表示失败或中断，本轮丢弃
            ret = zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0);
            if (ret <= 0) {
                msg_ptr.reset();
                continue;
            }

            // RPC 服务端模式：需要接收两帧数据，并查找对应注册的RPC服务，处理数据
            // 第1帧是 action 名称（存放在 msg_ptr）
            // 第2帧是请求体（读取到 msg1_ptr）
            if (mode_ == ZMQ_RPC_FUN) {
                std::shared_ptr<pzmq_data> msg1_ptr = std::make_shared<pzmq_data>();
                zmq_msg_recv(msg1_ptr->get(), zmq_socket_, 0);

                std::string retval;
                try {
                    // 访问 action -> 回调映射表需要加锁，避免并发注册/卸载造成竞态
                    // 注意：当前实现“持锁执行回调”，若回调较慢会拉长临界区
                    std::unique_lock<std::mutex> lock(zmq_fun_mtx_);
                    // 找到相应rpc服务，执行函数回调
                    // msg_ptr->string() 代表第一帧数据的服务名
                    retval = zmq_fun_.at(msg_ptr->string())(this, msg1_ptr);
                } catch (...) {
                    // action 不存在或回调异常时，统一返回错误字符串
                    retval = "NotAction";
                }

                // 将回调返回值作为 RPC 响应发回客户端
                zmq_send(zmq_socket_, retval.c_str(), retval.length(), 0);
                msg1_ptr.reset();
            } else {
                // SUB / PULL 普通消息模式：直接回调上层处理
                raw_call(this, msg_ptr);
            }

            msg_ptr.reset();
        }
    }
    void close_zmq()
    {
        zmq_close(zmq_socket_);
        zmq_ctx_destroy(zmq_ctx_);
        if ((mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN)) {
            if (!rpc_url_head_.empty()) {
                std::string socket_file = zmq_url_.substr(rpc_url_head_length);
                if (access(socket_file.c_str(), F_OK) == 0) {
                    remove(socket_file.c_str());
                }
            }
        }
        zmq_socket_ = NULL;
        zmq_ctx_    = NULL;
    }
    ~pzmq()
    {
        if (!zmq_socket_) {
            return;
        }
        flage_ = true;
        zmq_ctx_shutdown(zmq_ctx_);
        if (zmq_thread_) {
            zmq_thread_->join();
        }
        close_zmq();
    }
};
};  // namespace StackFlows