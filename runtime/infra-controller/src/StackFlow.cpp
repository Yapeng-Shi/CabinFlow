/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "StackFlow.h"
#include "sample_log.h"
#include <iostream>

using namespace StackFlows;

// 初始化RPC服务、事件队列和相关成员变量，启动事件循环线程
StackFlow::StackFlow(const std::string &unit_name) : unit_name_(unit_name), rpc_ctx_(std::make_unique<pzmq>(unit_name))
{
    // 给事件队列注册事件处理函数，建立事件类型到成员函数的分发表
    event_queue_.appendListener(LOCAL_EVENT::EVENT_NONE,
                                std::bind(&StackFlow::_none_event, this, std::placeholders::_1));
    event_queue_.appendListener(LOCAL_EVENT::EVENT_PAUSE, std::bind(&StackFlow::_pause, this, std::placeholders::_1));
    event_queue_.appendListener(LOCAL_EVENT::EVENT_EXIT, std::bind(&StackFlow::_exit, this, std::placeholders::_1));
    event_queue_.appendListener(LOCAL_EVENT::EVENT_SETUP, std::bind(&StackFlow::_setup, this, std::placeholders::_1));
    event_queue_.appendListener(LOCAL_EVENT::EVENT_TASKINFO,
                                std::bind(&StackFlow::_taskinfo, this, std::placeholders::_1));

    // 注册 RPC action，绑定到对应的成员函数，供外部通过 RPC 调用触发事件入队
    rpc_ctx_->register_rpc_action(
        "setup", std::bind(&StackFlow::_rpc_setup, this, std::placeholders::_1, std::placeholders::_2));
    rpc_ctx_->register_rpc_action(
        "pause", std::bind(&StackFlow::_rpc_pause, this, std::placeholders::_1, std::placeholders::_2));
    rpc_ctx_->register_rpc_action("exit",
                                  std::bind(&StackFlow::_rpc_exit, this, std::placeholders::_1, std::placeholders::_2));
    rpc_ctx_->register_rpc_action(
        "taskinfo", std::bind(&StackFlow::_rpc_taskinfo, this, std::placeholders::_1, std::placeholders::_2));

    status_.store(0);
    exit_flage_.store(false);
    // 启动事件循环线程，负责异步处理事件队列中的任务，避免在通信线程里直接跑耗时逻辑
    even_loop_thread_ = std::make_unique<std::thread>(std::bind(&StackFlow::even_loop, this));
    status_.store(1);
}

StackFlow::~StackFlow()
{
    while (1) {
        exit_flage_.store(true);
        event_queue_.enqueue(EVENT_NONE, nullptr);
        even_loop_thread_->join();

        auto iteam = llm_task_channel_.begin();
        if (iteam == llm_task_channel_.end()) {
            break;
        }
        sys_release_unit(iteam->first, "");
        iteam->second.reset();
        llm_task_channel_.erase(iteam->first);
    }
}

// 事件消费主循环，作用是异步处理事件队列中的任务
void StackFlow::even_loop()
{
    // 设置线程名称
    pthread_setname_np(pthread_self(), "even_loop");

    while (!exit_flage_.load()) {
        // 阻塞等待事件入队，事件到来时自动唤醒并处理事件，避免 CPU 空转和资源浪费
        event_queue_.wait();     // 阻塞等待事件
        event_queue_.process();  // 处理所有事件
    }
}

void StackFlow::_none_event(const std::shared_ptr<void> &arg)
{
    // std::shared_ptr<pzmq_data> originalPtr = std::static_pointer_cast<pzmq_data>(arg);
}

// RPC 服务接口，只负责把 setup 请求丢进事件队列异步处理
std::string StackFlow::_rpc_setup(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data)
{
    event_queue_.enqueue(EVENT_SETUP, data);
    return std::string("None");
}

// RPC 服务接口，“通用初始化入口”，作用是把一次 setup 请求转成可执行的任务上下文，并交给子类做具体业务初始化
int StackFlow::setup(const std::string &zmq_url, const std::string &raw)
{
    ALOGI("StackFlow::setup raw zmq_url:%s raw:%s", zmq_url.c_str(), raw.c_str());

    // 注册工作单元，获取工作 ID 和通信通道，设置推送 URL 和请求 ID，调用业务接口处理 setup 请求
    int workid_num      = sys_register_unit(unit_name_);
    std::string work_id = unit_name_ + "." + std::to_string(workid_num);
    auto task_channel   = get_channel(workid_num);
    // 外部用户推理接口 push url，远程客户端通信
    task_channel->set_push_url(zmq_url);
    task_channel->request_id_ = sample_json_str_get(raw, "request_id");
    task_channel->work_id_    = work_id;
    // 子类重写 setup 处理具体业务逻辑，默认实现返回错误信息
    if (setup(work_id, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data"))) {
        // setup 处理失败时，释放工作单元资源，避免资源泄漏
        sys_release_unit(workid_num, work_id);
    }
    return 0;
}

int StackFlow::setup(const std::string &work_id, const std::string &object, const std::string &data)
{
    ALOGI("StackFlow::setup");
    nlohmann::json error_body;
    error_body["code"]    = -18;
    error_body["message"] = "not have unit action!";
    send("None", "None", error_body, work_id);
    return -1;
}

// RPC 服务接口，只负责把 exit 请求丢进事件队列异步处理
std::string StackFlow::_rpc_exit(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data)
{
    event_queue_.enqueue(EVENT_EXIT, data);
    return std::string("None");
}

int StackFlow::exit(const std::string &zmq_url, const std::string &raw)
{
    ALOGI("StackFlow::exit raw");
    std::string work_id = sample_json_str_get(raw, "work_id");
    try {
        auto task_channel = get_channel(sample_get_work_id_num(work_id));
        task_channel->set_push_url(zmq_url);
    } catch (...) {
    }
    if (exit(work_id, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data")) == 0) {
        return (int)sys_release_unit(-1, work_id);
    }
    return 0;
}

int StackFlow::exit(const std::string &work_id, const std::string &object, const std::string &data)
{
    ALOGI("StackFlow::exit");

    nlohmann::json error_body;
    error_body["code"]    = -18;
    error_body["message"] = "not have unit action!";
    send("None", "None", error_body, work_id);
    return 0;
}

// // RPC 服务接口，只负责把 pause 请求丢进事件队列异步处理
std::string StackFlow::_rpc_pause(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data)
{
    event_queue_.enqueue(EVENT_PAUSE, data);
    return std::string("None");
}

void StackFlow::pause(const std::string &zmq_url, const std::string &raw)
{
    ALOGI("StackFlow::pause raw");
    std::string work_id = sample_json_str_get(raw, "work_id");
    try {
        auto task_channel = get_channel(sample_get_work_id_num(work_id));
        task_channel->set_push_url(zmq_url);
    } catch (...) {
    }
    pause(work_id, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data"));
}

void StackFlow::pause(const std::string &work_id, const std::string &object, const std::string &data)
{
    ALOGI("StackFlow::pause");

    nlohmann::json error_body;
    error_body["code"]    = -18;
    error_body["message"] = "not have unit action!";
    send("None", "None", error_body, work_id);
}

// RPC 服务接口，只负责把 taskinfo 请求丢进事件队列异步处理
std::string StackFlow::_rpc_taskinfo(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &data)
{
    event_queue_.enqueue(EVENT_TASKINFO, data);
    return std::string("None");
}

void StackFlow::taskinfo(const std::string &zmq_url, const std::string &raw)
{
    std::string work_id = sample_json_str_get(raw, "work_id");
    try {
        auto task_channel = get_channel(sample_get_work_id_num(work_id));
        task_channel->set_push_url(zmq_url);
    } catch (...) {
    }
    taskinfo(work_id, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data"));
}

void StackFlow::taskinfo(const std::string &work_id, const std::string &object, const std::string &data)
{
    nlohmann::json error_body;
    error_body["code"]    = -18;
    error_body["message"] = "not have unit action!";
    send("None", "None", error_body, work_id);
}

// RPC 服务接口：注册工作单元，获取工作 ID 和通信通道，设置推送 URL 和请求 ID，返回工作 ID 供后续使用
// 每个TASK 对应唯一的 work id, connect url
int StackFlow::sys_register_unit(const std::string &unit_name)
{
    int work_id_number;
    std::string str_port;  // 接口返回的工作 ID 字符串，包含整数部分和其他信息，需要解析出整数部分作为 work_id_number
    std::string out_port;  // 接口返回的输出端口 URL，供通信通道使用
    std::string inference_port;  // 接口返回的推理服务端口 URL，供通信通道使用

    // 调用系统接口注册工作单元，获取工作 ID 和通信通道，设置推送 URL 和请求 ID，返回工作 ID 供后续使用
    unit_call("sys", "register_unit", unit_name, [&](const std::shared_ptr<StackFlows::pzmq_data> &pzmg_msg) {
        // work id, connect url
        // pzmq_data::set_param(unit_info->output_url, unit_info->inference_url)
        str_port = pzmg_msg->get_param(1);
        // pub
        out_port = pzmg_msg->get_param(0, str_port);
        // 外部用户推理接口
        inference_port = pzmg_msg->get_param(1, str_port);

        // unit_info->port_
        str_port = pzmg_msg->get_param(0);
    });
    work_id_number = std::stoi(str_port);
    ALOGI("work_id_number:%d, out_port:%s, inference_port:%s ", work_id_number, out_port.c_str(),
          inference_port.c_str());
    // 创建 llm_channel_obj 通信对象，管理这个 work_id 的通信状态，并存入 llm_task_channel_ 以供后续使用
    llm_task_channel_[work_id_number] = std::make_shared<llm_channel_obj>(out_port, inference_port, unit_name_);
    return work_id_number;
}

// RPC 服务接口：释放工作单元资源，并清理通信通道
bool StackFlow::sys_release_unit(int work_id_num, const std::string &work_id)
{
    std::string _work_id;
    int _work_id_num;
    if (work_id.empty()) {
        _work_id     = sample_get_work_id(work_id_num, unit_name_);
        _work_id_num = work_id_num;
    } else {
        _work_id     = work_id;
        _work_id_num = sample_get_work_id_num(work_id);
    }
    // 调用节点注册管理服务中release_unit，rpc服务
    unit_call("sys", "release_unit", _work_id);
    llm_task_channel_[_work_id_num].reset();
    llm_task_channel_.erase(_work_id_num);
    ALOGI("release work_id %s success", _work_id.c_str());
    return false;
}

std::string StackFlow::sys_sql_select(const std::string &key)
{
}

void StackFlow::sys_sql_set(const std::string &key, const std::string &val)
{
    nlohmann::json out_body;
    out_body["key"] = key;
    out_body["val"] = val;
    // 对节点注册管理的设置
    unit_call("sys", "sql_set", out_body.dump());
}

void StackFlow::sys_sql_unset(const std::string &key)
{
    unit_call("sys", "sql_unset", key);
}
