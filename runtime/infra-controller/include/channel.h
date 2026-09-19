#pragma once

// #define __cplusplus 1

#include <semaphore.h>
#include <unistd.h>
#include <iostream>
// #define CONFIG_SUPPORTTHREADSAFE 0

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

#define LLM_NO_ERROR std::string("")
#define LLM_NONE     std::string("None")
namespace StackFlows {

// 单个任务的通信通道管理器，把这个任务在系统里的收发链路统一封装起来
class llm_channel_obj {
private:
    // 通过 work_id 订阅的 zmq 实例，支持同一单元同时连接多个服务端（不同 work_id 代表不同服务）
    std::unordered_map<int, std::shared_ptr<pzmq>> zmq_;
    // 通过 url 订阅的 zmq 实例，支持同一单元同时连接多个服务端（不同 url 代表不同服务）
    std::atomic<int> zmq_url_index_;
    // 通过 subscriber 订阅的 zmq 实例，支持同一单元同时连接多个服务端（不同 url 代表不同服务）
    std::unordered_map<std::string, int> zmq_url_map_;

public:
    std::string unit_name_;      // 单元名称（用于日志输出和调试）
    bool enoutput_;              // 是否启用输出（控制是否向用户发送消息）
    bool enstream_;              // 是否启用流式输出（控制是否分块发送消息）
    std::string request_id_;     // 当前请求 ID，rpc的请求标识（用于关联请求和响应）
    std::string work_id_;        // 当前工作 ID，代表连接的服务标识（用于区分不同服务的连接）
    std::string inference_url_;  // 推理服务 URL，pub/sub（用于连接推理服务的地址）
    std::string publisher_url_;  // 发布服务 URL，pub/sub（用于连接发布服务的地址）
    std::string output_url_;     // 输出服务 URL，pull/push（用于连接输出服务的地址）
    std::string publisher_url;   // 发布服务 URL，pub/sub（用于连接发布服务的地址）

    llm_channel_obj(const std::string &_publisher_url, const std::string &inference_url, const std::string &unit_name);
    ~llm_channel_obj();
    inline void set_output(bool flage)
    {
        enoutput_ = flage;
    }
    inline bool get_output()
    {
        return enoutput_;
    }
    inline void set_stream(bool flage)
    {
        enstream_ = flage;
    }
    inline bool get_stream()
    {
        return enstream_;
    }
    void subscriber_event_call(const std::function<void(const std::string &, const std::string &)> &call, pzmq *_pzmq,
                               const std::shared_ptr<pzmq_data> &raw);
    int subscriber_work_id(const std::string &work_id,
                           const std::function<void(const std::string &, const std::string &)> &call);
    void stop_subscriber_work_id(const std::string &work_id);
    void subscriber(const std::string &zmq_url, const pzmq::msg_callback_fun &call);
    void stop_subscriber(const std::string &zmq_url);
    int send_raw_to_pub(const std::string &raw);
    int send_raw_to_usr(const std::string &raw);
    void set_push_url(const std::string &url);
    void cear_push_url();
    static int send_raw_for_url(const std::string &zmq_url, const std::string &raw);

    int send(const std::string &object, const nlohmann::json &data, const std::string &error_msg,
             const std::string &work_id = "")
    {
        nlohmann::json out_body;
        out_body["request_id"] = request_id_;
        out_body["work_id"]    = work_id.empty() ? work_id_ : work_id;
        out_body["created"]    = time(NULL);
        out_body["object"]     = object;
        out_body["data"]       = data;
        if (error_msg.empty()) {
            out_body["error"]["code"]    = 0;
            out_body["error"]["message"] = "";
        } else
            out_body["error"] = error_msg;

        std::string out = out_body.dump();
        out += "\n";

        send_raw_to_pub(out);
        if (enoutput_) {
            return send_raw_to_usr(out);
        }
        return 0;
    }
};
}  // namespace StackFlows