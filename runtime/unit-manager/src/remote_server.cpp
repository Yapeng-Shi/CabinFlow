
/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <iostream>
#include <vector>
#include "all.h"
#include "remote_server.h"
#include "zmq_bus.h"
#include <simdjson.h>
#include <cstring>
#include <StackFlowUtil.h>
#include "json.hpp"
#include "remote_action.h"

using namespace StackFlows;

std::atomic<int>
    work_id_number_counter;   // 全局工作 ID 计数器，负责为每个新注册的单元分配一个唯一的 ID，初始值从配置文件中读取
int port_list_start;          // ZMQ 端口池起始端口号，从配置文件中读取
std::vector<bool> port_list;  // 端口占用位图，用来管理可分配的 ZMQ 端口池
std::unique_ptr<pzmq> sys_rpc_server_;  // 系统 RPC 服务端对象，负责监听和处理来自业务节点的 RPC 请求

// 系统 SQL 查询接口，从全局 KV 数据库中读取指定 key 的值并返回
std::string sys_sql_select(const std::string &key)
{
    std::string out;
    SAFE_READING(out, std::string, key);
    return out;
}

// 系统 SQL 设置接口，向全局 KV 数据库中设置指定 key 的值
void sys_sql_set(const std::string &key, const std::string &val)
{
    SAFE_SETTING(key, val);
}

// 系统 SQL 删除接口，从全局 KV 数据库中删除指定 key 的条目
void sys_sql_unset(const std::string &key)
{
    SAFE_ERASE(key);
}

// 单元注册，给一个新注册的业务单元分配系统资源，并生成它的通信信息
unit_data *sys_allocate_unit(const std::string &unit)
{
    // 创建一个新的 unit_data 对象，并为它分配一个唯一的 work_id（格式为 "unit.port"）
    // 和两个 ZMQ 端口（分别用于内部通信和外部用户推理接口）
    unit_data *unit_p = new unit_data();
    {
        // work_id_number:0
        unit_p->port_     = work_id_number_counter++;
        std::string ports = std::to_string(unit_p->port_);
        // llm.0
        unit_p->work_id = unit + "." + ports;
    }

    // 分配 ZMQ 端口并生成对应的 output_url，分别用于内部通信和外部用户推理接口
    {
        int port;
        for (size_t i = 0; i < port_list.size(); i++) {
            if (!port_list[i]) {
                port         = port_list_start + i;
                port_list[i] = true;
                break;
            }
        }
        std::string ports      = std::to_string(port);
        std::string zmq_format = zmq_s_format;
        if (zmq_s_format.find("sock") != std::string::npos) {
            zmq_format += ".";
            zmq_format += unit;
            zmq_format += ".output_url";
        }
        std::vector<char> buff(zmq_format.length() + ports.length(), 0);
        sprintf((char *)buff.data(), zmq_format.c_str(), port);
        std::string zmq_s_url = std::string((char *)buff.data());
        unit_p->output_url    = zmq_s_url;
    }

    // 分配 ZMQ 端口并生成 inference_url，供外部用户调用推理接口使用
    {
        int port;
        for (size_t i = 0; i < port_list.size(); i++) {
            if (!port_list[i]) {
                port         = port_list_start + i;
                port_list[i] = true;
                break;
            }
        }
        std::string ports      = std::to_string(port);
        std::string zmq_format = zmq_s_format;
        if (zmq_s_format.find("sock") != std::string::npos) {
            zmq_format += ".";
            zmq_format += unit;
            zmq_format += ".inference_url";
        }
        std::vector<char> buff(zmq_format.length() + ports.length(), 0);
        sprintf((char *)buff.data(), zmq_format.c_str(), port);
        std::string zmq_s_url = std::string((char *)buff.data());
        // 初始化 unit_data 对象的推理通信通道，创建一个 ZMQ PUB 连接，供外部用户发送推理请求
        unit_p->init_zmq(zmq_s_url);
    }

    // 将分配的 unit_data 对象存储到全局 KV 数据库中
    SAFE_SETTING(unit_p->work_id, unit_p);
    // key: llm.0.out_port, value: ipc:///tmp/llm/5010.sock.llm.output_url
    SAFE_SETTING(unit_p->work_id + ".out_port", unit_p->output_url);
    return unit_p;
}

// 单元释放：清理系统资源
int sys_release_unit(const std::string &unit)
{
    unit_data *unit_p = NULL;
    SAFE_READING(unit_p, unit_data *, unit);
    if (NULL == unit_p) {
        return -1;
    }

    int port;
    sscanf(unit_p->output_url.c_str(), zmq_s_format.c_str(), &port);
    port_list[port - port_list_start] = false;
    sscanf(unit_p->inference_url.c_str(), zmq_s_format.c_str(), &port);
    port_list[port - port_list_start] = false;

    // 释放 unit_data 对象并从全局 KV 数据库中删除相关条目
    delete unit_p;
    SAFE_ERASE(unit);
    SAFE_ERASE(unit + ".out_port");
    return 0;
}

// RPC 单元分配接口，供业务节点注册新的单元并获取通信信息
// pzmq *_pzmq：当前触发回调的 pzmq 实例指针；
// const std::shared_ptr<pzmq_data> &raw：本次收到的消息体数据（请求参数）
std::string rpc_allocate_unit(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    // 单元注册
    unit_data *unit_info = sys_allocate_unit(raw->string());
    // 对业务节点的 RPC 响应
    return pzmq_data::set_param(std::to_string(unit_info->port_),
                                pzmq_data::set_param(unit_info->output_url, unit_info->inference_url));
}

// RPC 单元释放接口，供业务节点释放不再使用的单元资源
std::string rpc_release_unit(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    sys_release_unit(raw->string());
    return "Success";
}

// RPC 数据库查询接口，供业务节点查询系统资源信息
std::string rpc_sql_select(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    return sys_sql_select(raw->string());
}

// RPC 数据库设置接口，供业务节点设置系统资源信息
std::string rpc_sql_set(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    std::string key = sample_json_str_get(raw->string(), "key");
    std::string val = sample_json_str_get(raw->string(), "val");
    if (key.empty()) return "False";
    sys_sql_set(key, val);
    return "Success";
}

// RPC 数据库删除接口，供业务节点删除系统资源信息
std::string rpc_sql_unset(pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    sys_sql_unset(raw->string());
    return "Success";
}

// 远程服务器工作线程，初始化 RPC 服务并注册系统接口，等待处理 RPC 请求
void remote_server_work()
{
    int port_list_end;  // ZMQ 端口池结束端口号，从配置文件中读取

    SAFE_READING(work_id_number_counter, int, "config_work_id");
    // port 本质是文件名
    SAFE_READING(port_list_start, int, "config_zmq_min_port");
    SAFE_READING(port_list_end, int, "config_zmq_max_port");

    // 初始化端口池，大小为 (port_list_end - port_list_start)，初始值为 false 表示端口可用
    port_list.resize(port_list_end - port_list_start, 0);

    // 通过 pzmq 创建名为 sys 的 RPC 服务端入口，并注册系统接口函数，等待处理来自业务节点的 RPC 请求
    sys_rpc_server_ = std::make_unique<pzmq>("sys");
    sys_rpc_server_->register_rpc_action("sql_select",
                                         std::bind(rpc_sql_select, std::placeholders::_1, std::placeholders::_2));
    sys_rpc_server_->register_rpc_action("register_unit",
                                         std::bind(rpc_allocate_unit, std::placeholders::_1, std::placeholders::_2));
    sys_rpc_server_->register_rpc_action("release_unit",
                                         std::bind(rpc_release_unit, std::placeholders::_1, std::placeholders::_2));
    sys_rpc_server_->register_rpc_action("sql_set",
                                         std::bind(rpc_sql_set, std::placeholders::_1, std::placeholders::_2));
    sys_rpc_server_->register_rpc_action("sql_unset",
                                         std::bind(rpc_sql_unset, std::placeholders::_1, std::placeholders::_2));
}

void remote_server_stop_work()
{
    sys_rpc_server_.reset();
}

void usr_print_error(const std::string &request_id, const std::string &work_id, const std::string &error_msg,
                     int zmq_out)
{
    // 数据包设计
    nlohmann::json out_body;
    out_body["request_id"] = request_id;
    out_body["work_id"]    = work_id;
    out_body["created"]    = time(NULL);
    out_body["error"]      = nlohmann::json::parse(error_msg);
    out_body["object"]     = std::string("None");
    out_body["data"]       = std::string("None");
    std::string out        = out_body.dump();
    // 异常数据发送 PUSH发送给zmq_bus -> PULL接收数据 -> send TCP转发给客户端
    zmq_com_send(zmq_out, out);
}

std::mutex unit_action_match_mtx;   // 任务分发接口互斥锁
simdjson::ondemand::parser parser;  // SIMDJSON 解析器对象，把 JSON 字符串解析成可操作的 JSON 对象
// 远程函数调用接口，供任务分发时调用远程函数处理业务逻辑
// 参数为 com_id 和 JSON 格式的请求数据
typedef int (*sys_fun_call)(int, const nlohmann::json &);

// 动态任务分发接口
void unit_action_match(int com_id, const std::string &json_str)
{
    // 给 unit_action_match 加锁，保证同一时间只有一个线程在处理任务分发，避免并发访问导致的资源冲突
    std::lock_guard<std::mutex> guard(unit_action_match_mtx);
    // json数据 string类型转化为 simdjson可操作的类型
    simdjson::padded_string json_string(json_str);
    simdjson::ondemand::document doc;

    // 从 JSON 字符串解析出 request_id、work_id 和 action 字段，进行基本的格式校验
    auto error = parser.iterate(json_string).get(doc);
    ALOGI("json format :%s", json_str.c_str());
    if (error) {
        ALOGE("json format error:%s", json_str.c_str());
        // 给客户端发送异常数据
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    std::string request_id;
    error = doc["request_id"].get_string(request_id);
    if (error) {
        ALOGE("miss request_id, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    std::string work_id;
    error = doc["work_id"].get_string(work_id);
    if (error) {
        ALOGE("miss work_id, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    if (work_id.empty()) work_id = "sys";
    std::string action;
    error = doc["action"].get_string().get(action);
    if (error) {
        ALOGE("miss action, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }

    // 把 work_id 按 "." 手动切分成数组存在 work_id_fragment
    std::vector<std::string> work_id_fragment;  // work_id 以 "." 分隔的字符串数组，方便根据 action 类型进行任务分发
    std::string fragment;  // work_id 的临时字符串片段，用于解析 work_id 中的不同部分，如单元类型和端口号等
    for (auto c : work_id) {
        if (c != '.') {
            fragment.push_back(c);
        } else {
            work_id_fragment.push_back(fragment);
            fragment.clear();
        }
    }
    if (fragment.length()) work_id_fragment.push_back(fragment);

    // action 是 "inference"，则将消息转发到对应的 ZMQ 发布通道进行处理
    // 对任务分发 -> 解析action -> inference_url -> pub 把客户端请求发给业务节点
    if (action == "inference") {
        char zmq_push_url[128];
        int post = sprintf(zmq_push_url, zmq_c_format.c_str(), com_id);
        std::string inference_raw_data;  // 按约定协议打包数据，前面是 zmq_push_url，后面是原始 json_str 数据
        // PUSH-PULL的url打包到json数据里
        post = sprintf(inference_raw_data.data(), "{\"zmq_com\":\"");
        post += sprintf(inference_raw_data.data() + post, "%s", zmq_push_url);
        post += sprintf(inference_raw_data.data() + post, "\",");
        memcpy(inference_raw_data.data() + post, json_str.data() + 1, json_str.length() - 1);
        // 把推理请求发送到业务节点的 ZMQ 发布通道，供业务节点订阅处理
        int ret = zmq_bus_publisher_push(work_id, inference_raw_data);
        if (ret) {
            usr_print_error(request_id, work_id, "{\"code\":-4, \"message\":\"inference data push false\"}", com_id);
        }
    }
    // 其他action类型的任务分发，调用远程函数接口 remote_call 处理 action 对应的业务逻辑，并将结果发送给客户端
    else {
        if ((work_id_fragment[0].length() != 0) && (remote_call(com_id, json_str) != 0)) {
            usr_print_error(request_id, work_id, "{\"code\":-9, \"message\":\"unit call false\"}", com_id);
        }
    }
}