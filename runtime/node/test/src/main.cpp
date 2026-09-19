/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "StackFlow.h"
#include "channel.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <stdexcept>
#include <iostream>
using namespace StackFlows;
using json = nlohmann::json;

// 实现业务节点：离线大模型语音智能交互：asr模块，LLM模块，TTS模块等

// 全局变量，标志程序是否需要退出，初始值为0，表示程序正常运行；
int main_exit_flage = 0;
// 当接收到 SIGTERM 或 SIGINT 信号时，将该变量设置为1，表示程序需要退出
static void __sigint(int iSigNo)
{
    main_exit_flage = 1;
}
// 定义一个函数类型，表示模型推理结果的回调函数，接受两个参数：data（模型推理结果数据）和finish（表示是否完成推理的标志）
typedef std::function<void(const std::string &data, bool finish)> task_callback_t;

// 单个 LLM 任务的抽象；模型加载，模型推理相关任务操作
class llm_task {
private:
public:
    std::string model_;                // 模型标识，代表当前任务使用的模型，可以是模型名称、路径或其他唯一标识
    std::string response_format_;      // 模型推理输出格式，控制模型输出内容的结构和格式，可能包含是否流式输出等信息
    std::vector<std::string> inputs_;  // 模型推理输入列表，代表当前任务需要处理的输入数据，可以是多个输入项的集合
    task_callback_t out_callback_;     // 模型推理输出回调函数，模型推理完成后会调用这个函数将结果发送给用户
    bool enoutput_;                    // 是否启用输出，控制是否向用户发送模型推理结果
    bool enstream_;                    // 是否启用流式输出，控制模型推理结果是一次性发送还是分块发送（流式）

    void set_output(task_callback_t out_callback)
    {
        out_callback_ = out_callback;
    }

    // 解析json数据，从中提取模型配置参数，并根据这些参数设置模型推理的输出格式、是否启用输出和流式输出等配置
    bool parse_config(const nlohmann::json &config_body)
    {
        try {
            model_           = config_body.at("model");
            response_format_ = config_body.at("response_format");
            enoutput_        = config_body.at("enoutput");
            if (config_body.contains("input")) {
                if (config_body["input"].is_string()) {
                    inputs_.push_back(config_body["input"].get<std::string>());
                } else if (config_body["input"].is_array()) {
                    for (auto _in : config_body["input"]) {
                        inputs_.push_back(_in.get<std::string>());
                    }
                }
            }
        } catch (...) {
            return true;
        }
        enstream_ = (response_format_.find("stream") != std::string::npos);
        return false;
    }

    int load_model(const nlohmann::json &config_body)
    {
        if (parse_config(config_body)) {
            return -1;
        }
        return 0;
    }

    // 模型推理接口，接受输入数据，执行模型推理，并通过 out_callback_ 将结果发送给用户
    void inference(const std::string &msg)
    {
        // 缺失大模型推理后的输出，输出结果传递给out_callback_回调函数
        if (out_callback_) {
            out_callback_(msg, false);
            out_callback_(std::string("hello"), false);

            out_callback_(std::string(""), true);
        }
    }

    llm_task(const std::string &workid)
    {
    }

    void start()
    {
    }

    void stop()
    {
    }

    ~llm_task()
    {
        stop();
    }
};

// 把 StackFlow 提供的通用 RPC/通道框架，落地成一个具体的 LLM 业务节点实现
// 管理多个 llm_task 对象，每个对象代表一个独立的推理任务，负责解析输入数据、执行推理并通过回调函数输出结果
// 设置流式推理配置，是否与外部用户信息交互
// 配置模型参数，模型推理输出如何回调处理，订阅哪些节点数据
class llm_llm : public StackFlow {
private:
    int task_count_;  // llm_task 任务数量（线程数）

    // 任务对象映射表，key 是 work_id_num，value 是对应的 llm_task 对象指针
    std::unordered_map<int, std::shared_ptr<llm_task>> llm_task_;

public:
    // 构造函数，初始化 StackFlow 基类，设置任务数量
    llm_llm() : StackFlow("llm")
    {
        task_count_ = 3;
    }

    // 把模型推理结果按协议封装后，通过当前任务通道回传给客户端
    void task_output(const std::weak_ptr<llm_task> llm_task_obj_weak,
                     const std::weak_ptr<llm_channel_obj> llm_channel_weak, const std::string &data, bool finish)
    {
        // 如果 llm_task_obj 或 llm_channel 已经被销毁（可能是因为任务完成或连接断开），则不执行回调逻辑，直接返回
        // weak::lock() 核心作用：
        // 1、检查 weak_ptr 指向的对象是否还活着（没被销毁）
        // 2、如果活着：返回一个有效的shared_ptr，持有对象，防止中途被释放
        // 3、如果死了：返回一个空的 shared_ptr
        // 4、这个操作是原子的、线程安全的
        auto llm_task_obj = llm_task_obj_weak.lock();
        auto llm_channel  = llm_channel_weak.lock();
        if (!(llm_task_obj && llm_channel)) {
            return;
        }

        // 流式输出：每次回调发送一个数据块，数据块格式包含 index（数据块序号），delta（数据块内容），finish（是否完成）
        if (llm_channel->enstream_) {
            static int count = 0;
            nlohmann::json data_body;
            data_body["index"] = count++;
            data_body["delta"] = data;
            if (!finish)
                data_body["delta"] = data;
            else
                data_body["delta"] = std::string("");
            data_body["finish"] = finish;
            if (finish) count = 0;

            llm_channel->send(llm_task_obj->response_format_, data_body, LLM_NO_ERROR);
        }
        // 非流式输出：只在推理完成时回调一次，判断是否是最后一个字段，如果是最后一个字段则发送完整响应，否则不发送
        else if (finish) {
            llm_channel->send(llm_task_obj->response_format_, data, LLM_NO_ERROR);
        }
    }

    // 对用户输入数据进行处理，解析数据中的 object 和 data 字段
    // 根据 object 字段的内容决定如何处理 data 数据，并将结果通过回调函数发送给用户
    void task_user_data(const std::weak_ptr<llm_task> llm_task_obj_weak,
                        const std::weak_ptr<llm_channel_obj> llm_channel_weak, const std::string &object,
                        const std::string &data)
    {
        nlohmann::json error_body;
        auto llm_task_obj = llm_task_obj_weak.lock();
        auto llm_channel  = llm_channel_weak.lock();
        if (!(llm_task_obj && llm_channel)) {
            error_body["code"]    = -11;
            error_body["message"] = "Model run failed.";
            send("None", "None", error_body, unit_name_);
            return;
        }
        if (data.empty() || (data == "None")) {
            error_body["code"]    = -24;
            error_body["message"] = "The inference data is empty.";
            send("None", "None", error_body, unit_name_);
            return;
        }
        const std::string *next_data = &data;
        int ret;
        std::string tmp_msg;
        // 判断是否是流式推理输入，如果是流式输入则调用 decode_stream 进行数据解码，获取完整的输入数据后再进行推理
        if (object.find("stream") != std::string::npos) {
            static std::unordered_map<int, std::string> stream_buff;
            try {
                if (decode_stream(data, tmp_msg, stream_buff)) {
                    return;
                };
            } catch (...) {
                stream_buff.clear();
                error_body["code"]    = -25;
                error_body["message"] = "Stream data index error.";
                send("None", "None", error_body, unit_name_);
                return;
            }
            next_data = &tmp_msg;
        }

        // 处理 json 数据后，获取 delta
        llm_task_obj->inference((*next_data));
    }

    // 由基类StackFlow 中的接口处理JSON数据后，获取JSON数据中work_id，object，data
    // 配置模型参数，模型推理输出如何回调处理，订阅哪些节点数据
    int setup(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        nlohmann::json error_body;
        if ((llm_task_channel_.size() - 1) == task_count_) {
            error_body["code"]    = -21;
            error_body["message"] = "task full";
            send("None", "None", error_body, unit_name_);
            return -1;
        }
        int work_id_num = sample_get_work_id_num(work_id);
        // 根据 work_id 获取对应的通信通道对象 llm_channel_obj，后续通过这个对象与外部用户进行通信
        auto llm_channel  = get_channel(work_id);
        auto llm_task_obj = std::make_shared<llm_task>(work_id);  // 创建 llm_task 对象，传入 work_id 以便后续使用
        nlohmann::json config_body;
        try {
            config_body = nlohmann::json::parse(data);
        } catch (...) {
            error_body["code"]    = -2;
            error_body["message"] = "json format error.";
            send("None", "None", error_body, unit_name_);
            return -2;
        }
        int ret = llm_task_obj->load_model(config_body);
        if (ret == 0) {
            // 设置与外部用户通信
            llm_channel->set_output(true);
            // 设置流式传输
            llm_channel->set_stream(llm_task_obj->enstream_);
            // 设置模型推理输出回调函数
            // 绑定到 llm_task_obj 的 out_callback_，回调函数会将推理结果通过 llm_channel 发送给用户
            llm_task_obj->set_output(std::bind(&llm_llm::task_output, this, std::weak_ptr<llm_task>(llm_task_obj),
                                               std::weak_ptr<llm_channel_obj>(llm_channel), std::placeholders::_1,
                                               std::placeholders::_2));
            // 设置订阅哪些节点数据（这里是默认订阅空节点：代码订阅inference输入）
            // 跨对象回调的安全设计，确保回调不延长对象生命周期，并避免循环引用风险
            // shared_ptr，引用计数+1，对llm_channel的生命周期延长了，
            // 主线程 channel创建，
            // 子线程 回调处理相关，/shared_ptr，引用计数+1，对llm_channel的生命周期延长了
            // 主线程channel析构了，但是llm_channel是没有真正析构掉，会造成资源泄露
            // 析构了一半，底层系统函数通信相关的析构了，没办法通信了，回调函数处理channel通信时，会发送异常

            // 循环引用的问题，
            // weak_ptr，不会添加引用计数，llm_channel外部的变量，如果外部的变量析构了，这块地址就不存在，
            // 回调函数里就没必要通信了

            // 创建一个订阅，回调函数是 llm_llm::task_user_data，回调函数会把用户输入数据传给 llm_task_obj 进行处理
            llm_channel->subscriber_work_id(
                "",
                std::bind(&llm_llm::task_user_data, this, std::weak_ptr<llm_task>(llm_task_obj),
                          std::weak_ptr<llm_channel_obj>(llm_channel), std::placeholders::_1, std::placeholders::_2));

            // 管理 llm_task 对象生命周期，存储到 llm_task_ 中，key 是 work_id_num，value 是 llm_task_obj
            llm_task_[work_id_num] = llm_task_obj;
            send("None", "None", LLM_NO_ERROR, work_id);

            return 0;
        } else {
            error_body["code"]    = -5;
            error_body["message"] = "Model loading failed.";
            send("None", "None", error_body, unit_name_);
            return -1;
        }
    }

    // 获取模型推理任务信息，如果 work_id 是 None 或者无效，则返回当前所有任务的列表；
    // 如果 work_id 有效，则返回对应任务的详细信息
    void taskinfo(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        nlohmann::json req_body;
        int work_id_num = sample_get_work_id_num(work_id);
        if (WORK_ID_NONE == work_id_num) {
            std::vector<std::string> task_list;
            std::transform(llm_task_channel_.begin(), llm_task_channel_.end(), std::back_inserter(task_list),
                           [](const auto task_channel) { return task_channel.second->work_id_; });
            req_body = task_list;
            send("llm.tasklist", req_body, LLM_NO_ERROR, work_id);
        } else {
            if (llm_task_.find(work_id_num) == llm_task_.end()) {
                req_body["code"]    = -6;
                req_body["message"] = "Unit Does Not Exist";
                send("None", "None", req_body, work_id);
                return;
            }
            auto llm_task_obj           = llm_task_[work_id_num];
            req_body["model"]           = llm_task_obj->model_;
            req_body["response_format"] = llm_task_obj->response_format_;
            req_body["enoutput"]        = llm_task_obj->enoutput_;
            req_body["inputs"]          = llm_task_obj->inputs_;
            send("llm.taskinfo", req_body, LLM_NO_ERROR, work_id);
        }
    }

    // 终止指定work id的task资源（关闭模型推理，关闭订阅信道等）
    int exit(const std::string &work_id, const std::string &object, const std::string &data) override
    {
        nlohmann::json error_body;
        int work_id_num = sample_get_work_id_num(work_id);
        if (llm_task_.find(work_id_num) == llm_task_.end()) {
            error_body["code"]    = -6;
            error_body["message"] = "Unit Does Not Exist";
            send("None", "None", error_body, work_id);
            return -1;
        }
        llm_task_[work_id_num]->stop();
        auto llm_channel = get_channel(work_id_num);
        llm_channel->stop_subscriber("");
        llm_task_.erase(work_id_num);
        send("None", "None", LLM_NO_ERROR, work_id);
        return 0;
    }

    ~llm_llm()
    {
        while (1) {
            auto iteam = llm_task_.begin();
            if (iteam == llm_task_.end()) {
                break;
            }
            iteam->second->stop();
            get_channel(iteam->first)->stop_subscriber("");
            iteam->second.reset();
            llm_task_.erase(iteam->first);
        }
    }
};

int main(int argc, char *argv[])
{
    // 捕获 SIGTERM 和 SIGINT 信号，执行退出进程操作
    signal(SIGTERM, __sigint);
    signal(SIGINT, __sigint);

    // 创建/tmp/llm，便于进程间socket通信（Unix Domain Socket）
    // ipc:///tmp/llm/8000.sock
    mkdir("/tmp/llm", 0777);

    // 创建 llm_llm 对象，启动 LLM 业务节点
    llm_llm llm;
    while (!main_exit_flage) {
        sleep(1);
    }
    return 0;
}