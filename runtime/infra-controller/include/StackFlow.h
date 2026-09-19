/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

// #define __cplusplus 1

#include <semaphore.h>
#include <unistd.h>
#include <iostream>

#include <string>
#include <list>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <eventpp/eventqueue.h>
#include <thread>
#include <memory>
#include "json.hpp"
#include <regex>
#include "pzmq.hpp"
#include "StackFlowUtil.h"
#include "channel.h"

namespace StackFlows {

// “业务节点基类框架”，封装通用的控制面和通信面能力，让你只需要在子类里写具体业务逻辑
class StackFlow {
public:
    typedef enum {  // 事件类型枚举，代表不同的事件类别
        EVENT_NONE = 0,
        EVENT_SETUP,
        EVENT_EXIT,
        EVENT_PAUSE,
        EVENT_TASKINFO,
    } LOCAL_EVENT;

    std::string unit_name_;  // 业务单元名（如 llm、asr、tts），代表当前服务的标识，通常用于注册和日志输出
    std::string
        request_id_;  // 当前正在处理请求的RPC请求的唯一标识（来自上游JSON），用于把异步处理后的响应与原始请求关联起来
    std::string out_zmq_url_;  // 当前请求对应的“用户回传 PUSH 地址”，send() 在未显式传入 zmq_url 时，默认发往这里

    std::atomic<bool> exit_flage_;  // 事件循环退出标志，false 表示事件循环继续运行，true 表示请求事件循环退出
    std::atomic<int> status_;       // 节点就绪状态：0 表示未就绪，1 表示就绪

    // 事件队列（控制面异步化核心）：
    // RPC 回调线程只负责入队，真正业务处理在 even_loop_thread_ 中执行，避免在通信线程里直接跑耗时逻辑
    // std::shared_ptr<void> 会擦除具体类型，void* 可以存储任意类型数据，使用时需要 static_pointer_cast 恢复原始类型
    eventpp::EventQueue<int, void(const std::shared_ptr<void> &)> event_queue_;
    // 事件消费线程：循环 wait/process event_queue_
    std::unique_ptr<std::thread> even_loop_thread_;

    // 当前业务节点的 RPC 上下文：
    // 负责注册并承载 setup/pause/exit/taskinfo 这些 RPC action
    std::unique_ptr<pzmq> rpc_ctx_;

    // 任务通信池（每个 work_id_num 对应一个通信通道对象）：
    // key   = work_id 的数字部分
    // value = 该任务的 llm_channel_obj（管理 pub/sub/push 等通道）
    // 作用：支持并发多任务、按任务隔离通信状态
    std::unordered_map<int, std::shared_ptr<llm_channel_obj>> llm_task_channel_;

    StackFlow(const std::string &unit_name);
    void even_loop();  // 事件循环函数，在线程中运行，负责等待和处理事件队列中的事件
    void _none_event(const std::shared_ptr<void> &arg);

    // 获取通信通道对象的模板方法，支持传入整数或字符串形式的 work_id，
    // 根据类型解析出整数形式的 work_id 并返回对应的通信对象指针
    template <typename T>
    std::shared_ptr<llm_channel_obj> get_channel(T workid)
    {
        int _work_id_num;
        if constexpr (std::is_same<T, int>::value) {
            _work_id_num = workid;
        } else if constexpr (std::is_same<T, std::string>::value) {
            _work_id_num = sample_get_work_id_num(workid);
        } else {
            return nullptr;
        }
        return llm_task_channel_.at(_work_id_num);
    }

    std::string _rpc_setup(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data);

    // 事件层的 setup 分发函数，把队列里的原始参数转成可用上下文，然后调用真正的 setup 逻辑
    void _setup(const std::shared_ptr<void> &arg)
    {
        // std::static_pointer_cast 的作用是把一个 std::shared_ptr 从一种类型安全地转换成另一种类型的 shared_ptr
        // 把“被擦除类型的事件参数”还原成真实的 pzmq_data 指针，方便后续读取消息内容
        std::shared_ptr<pzmq_data> originalPtr = std::static_pointer_cast<pzmq_data>(arg);
        // data->get_param(0), data->get_param(1)
        std::string zmq_url = originalPtr->get_param(0);
        std::string data    = originalPtr->get_param(1);

        request_id_  = sample_json_str_get(data, "request_id");
        out_zmq_url_ = zmq_url;

        // 根据当前状态决定是否处理请求，status_ == 1 表示系统已就绪，可以处理 setup 请求
        if (status_.load()) setup(zmq_url, data);
    };
    virtual int setup(const std::string &zmq_url, const std::string &raw);
    // 真正的 setup 业务逻辑函数，参数是解析好的 work_id/object/data，
    // 子类重写这个函数处理具体业务逻辑，默认实现返回错误信息
    virtual int setup(const std::string &work_id, const std::string &object, const std::string &data);

    std::string _rpc_exit(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data);
    void _exit(const std::shared_ptr<void> &arg)
    {
        std::shared_ptr<pzmq_data> originalPtr = std::static_pointer_cast<pzmq_data>(arg);
        std::string zmq_url                    = originalPtr->get_param(0);
        std::string data                       = originalPtr->get_param(1);
        request_id_                            = sample_json_str_get(data, "request_id");
        out_zmq_url_                           = zmq_url;
        if (status_.load()) exit(zmq_url, data);
    }
    virtual int exit(const std::string &zmq_url, const std::string &raw);
    virtual int exit(const std::string &work_id, const std::string &object, const std::string &data);

    std::string _rpc_pause(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data);
    void _pause(const std::shared_ptr<void> &arg)
    {
        std::shared_ptr<pzmq_data> originalPtr = std::static_pointer_cast<pzmq_data>(arg);
        std::string zmq_url                    = originalPtr->get_param(0);
        std::string data                       = originalPtr->get_param(0);
        request_id_                            = sample_json_str_get(data, "request_id");
        out_zmq_url_                           = zmq_url;
        if (status_.load()) pause(zmq_url, data);
    }
    virtual void pause(const std::string &zmq_url, const std::string &raw);
    virtual void pause(const std::string &work_id, const std::string &object, const std::string &data);

    std::string _rpc_taskinfo(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data);
    void _taskinfo(const std::shared_ptr<void> &arg)
    {
        std::shared_ptr<pzmq_data> originalPtr = std::static_pointer_cast<pzmq_data>(arg);
        // data->get_param(0), data->get_param(1)
        std::string zmq_url = originalPtr->get_param(0);
        std::string data    = originalPtr->get_param(1);
        request_id_         = sample_json_str_get(data, "request_id");
        out_zmq_url_        = zmq_url;
        if (status_.load()) taskinfo(zmq_url, data);
    }
    virtual void taskinfo(const std::string &zmq_url, const std::string &raw);
    virtual void taskinfo(const std::string &work_id, const std::string &object, const std::string &data);

    // 把流程中的错误步骤信息，直接返回给外部客户
    int send(const std::string &object, const nlohmann::json &data, const std::string &error_msg,
             const std::string &work_id, const std::string &zmq_url = "")
    {
        nlohmann::json out_body;
        out_body["request_id"] = request_id_;
        out_body["work_id"]    = work_id;
        out_body["created"]    = time(NULL);
        out_body["object"]     = object;
        out_body["data"]       = data;
        if (error_msg.empty()) {
            out_body["error"]["code"]    = 0;
            out_body["error"]["message"] = "";
        } else
            out_body["error"] = error_msg;

        if (zmq_url.empty()) {
            // 与外部用户通信，使用默认推送 URL，通常由 RPC 请求参数设置
            pzmq _zmq(out_zmq_url_, ZMQ_PUSH);
            std::string out = out_body.dump();
            out += "\n";
            return _zmq.send_data(out);
        } else {
            pzmq _zmq(zmq_url, ZMQ_PUSH);
            std::string out = out_body.dump();
            out += "\n";
            return _zmq.send_data(out);
        }
    }

    std::string sys_sql_select(const std::string &key);
    void sys_sql_set(const std::string &key, const std::string &val);
    void sys_sql_unset(const std::string &key);
    int sys_register_unit(const std::string &unit_name);
    template <typename T>
    bool sys_release_unit(T workid)
    {
        std::string _work_id;
        int _work_id_num;
        if constexpr (std::is_same<T, int>::value) {
            _work_id     = sample_get_work_id(workid, unit_name_);
            _work_id_num = workid;
        } else if constexpr (std::is_same<T, std::string>::value) {
            _work_id     = workid;
            _work_id_num = sample_get_work_id_num(workid);
        } else {
            return false;
        }
        pzmq _call("sys");
        _call.call_rpc_action("release_unit", _work_id, [](pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data) {});
        llm_task_channel_[_work_id_num].reset();
        llm_task_channel_.erase(_work_id_num);
        return false;
    }
    bool sys_release_unit(int work_id_num, const std::string &work_id);
    ~StackFlow();
};
};  // namespace StackFlows