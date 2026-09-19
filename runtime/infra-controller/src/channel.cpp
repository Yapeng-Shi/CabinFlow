#include "channel.h"
#include "sample_log.h"
#include <iostream>

using namespace StackFlows;

llm_channel_obj::llm_channel_obj(const std::string &_publisher_url, const std::string &inference_url,
                                 const std::string &unit_name)
    : unit_name_(unit_name), inference_url_(inference_url), publisher_url_(_publisher_url)
{
    // 预留负数槽位作为“固定通道”：
    // -1: 对外发布通道（PUB）
    // -2: 用户回传通道（PUSH，按需创建）
    zmq_url_index_ = -1000;  // URL 动态订阅通道从更小的负数开始分配，避免和固定槽位冲突

    // 初始化发布通道，供 send_raw_to_pub() 统一发送
    zmq_[-1] = std::make_shared<pzmq>(publisher_url_, ZMQ_PUB);

    // 用户回传通道默认不启用，收到有效 push url 后再在 set_push_url() 中创建
    zmq_[-2].reset();
}

llm_channel_obj::~llm_channel_obj()
{
    // 析构时打印日志，实际资源释放由 shared_ptr 自动完成
    std::cout << "llm_channel_obj 析构" << std::endl;
}

// 消息回调处理函数：从原始消息中提取 object 和 data 字段，并调用业务回调 call。
// 订阅消息统一回调：
// 1) 从原始报文中扫描 "action" 关键字（跳过被转义的情况）
// 2) 若判定为控制类消息，则尝试更新回传通道地址，并记录 request_id/work_id
// 3) 无论是否命中 action，最终都提取 object/data 交给上层业务回调
void llm_channel_obj::subscriber_event_call(const std::function<void(const std::string &, const std::string &)> &call,
                                            pzmq *_pzmq, const std::shared_ptr<pzmq_data> &raw)
{
    // 将 ZMQ 消息体转换为字符串，便于按 JSON 字段做轻量解析
    auto _raw = raw->string();

    // 查找 action 字段标记；注意这里是字符串搜索，不是完整 JSON 解析
    const char *user_inference_flage_str = "\"action\"";
    std::size_t pos                      = _raw.find(user_inference_flage_str);

    while (true) {
        // 未找到 action，结束扫描
        if (pos == std::string::npos) {
            break;
        } else if ((pos > 0) && (_raw[pos - 1] != '\\')) {
            // 命中的 action 不是转义文本的一部分，按有效控制字段处理
            // 读取 zmq_com 作为用户回传地址；若有更新则重建 PUSH 通道
            std::string zmq_com = sample_json_str_get(_raw, "zmq_com");
            if (!zmq_com.empty()) set_push_url(zmq_com);

            // 记录当前消息关联的请求与任务标识，供后续流程使用
            request_id_ = sample_json_str_get(_raw, "request_id");
            work_id_    = sample_json_str_get(_raw, "work_id");
            break;
        }

        // 继续向后查找，跳过当前命中位置
        pos = _raw.find(user_inference_flage_str, pos + sizeof(user_inference_flage_str));
    }

    // 提取业务负载并回调上层：
    // object: 语义类型/路由标识
    // data:   实际数据内容
    call(sample_json_str_get(_raw, "object"), sample_json_str_get(_raw, "data"));
}

void message_handler(pzmq *zmq_obj, const std::shared_ptr<pzmq_data> &data)
{
    std::cout << "Received: " << data->string() << std::endl;
}

// 根据 work_id 订阅对应的 ZMQ 服务。
// 设计上允许同一个单元同时订阅多个服务端：
// - work_id 形如 "name.id" 时，id 作为 zmq_ 的槽位键，独立维护连接
// - work_id 为空或不匹配规则时，退化到默认推理通道（id=0）
int llm_channel_obj::subscriber_work_id(const std::string &work_id,
                                        const std::function<void(const std::string &, const std::string &)> &call)
{
    int id_num;                             // 当前订阅通道在 zmq_ 中使用的键
    std::string subscriber_url;             // 最终用于建立 SUB 连接的地址
    std::regex pattern(R"((\w+)\.(\d+))");  // 匹配 "xxx.123" 结构，第二段数字作为通道 id
    std::smatch matches;                    // 用于存储正则匹配结果

    // 优先按 work_id 动态解析专属订阅地址
    if ((!work_id.empty()) && std::regex_match(work_id, matches, pattern)) {
        if (matches.size() == 3) {
            // matches[0]: 全匹配字符串; matches[1]: 前缀名；matches[2]: 数字 id
            id_num = std::stoi(matches[2].str());

            // 从配置系统查询该 work_id 对应的输出端口地址, 查询的是其他业务节点的 pub_url 地址
            std::string input_url_name = work_id + ".out_port";
            std::string input_url      = unit_call("sys", "sql_select", input_url_name);
            if (input_url.empty()) {
                // 查询失败时直接返回错误，避免创建无效连接
                return -1;
            }
            subscriber_url = input_url;
        }
    } else {
        // 未提供合法 work_id 时，使用默认推理通道，接收外部用户的数据
        id_num         = 0;
        subscriber_url = inference_url_;
    }

    // 创建（或覆盖）对应槽位的 SUB 连接：
    // 收到消息后统一转发到 subscriber_event_call，再由其提取 object/data 交给业务回调 call。
    zmq_[id_num] = std::make_shared<pzmq>(
        subscriber_url, ZMQ_SUB,
        std::bind(&llm_channel_obj::subscriber_event_call, this, call, std::placeholders::_1, std::placeholders::_2));

    return 0;
}

// 停止订阅对应 work_id 的 ZMQ 服务，清理资源
void llm_channel_obj::stop_subscriber_work_id(const std::string &work_id)
{
    int id_num;
    std::regex pattern(R"((\w+)\.(\d+))");
    std::smatch matches;
    if (std::regex_match(work_id, matches, pattern)) {
        if (matches.size() == 3) {
            // std::string part1 = matches[1].str();
            id_num = std::stoi(matches[2].str());
        }
    } else {
        id_num = 0;
    }
    if (zmq_.find(id_num) != zmq_.end()) zmq_.erase(id_num);
}

// 通用订阅接口，根据 zmq_url 直接创建 SUB 连接，适用于不区分 work_id 的场景
void llm_channel_obj::subscriber(const std::string &zmq_url, const pzmq::msg_callback_fun &call)
{
    zmq_url_map_[zmq_url]       = zmq_url_index_--;
    zmq_[zmq_url_map_[zmq_url]] = std::make_shared<pzmq>(zmq_url, ZMQ_SUB, call);
}

// 停止订阅对应 zmq_url 的 ZMQ 服务，清理资源
void llm_channel_obj::stop_subscriber(const std::string &zmq_url)
{
    if (zmq_url.empty()) {
        zmq_.clear();
        zmq_url_map_.clear();
    } else if (zmq_url_map_.find(zmq_url) != zmq_url_map_.end()) {
        zmq_.erase(zmq_url_map_[zmq_url]);
        zmq_url_map_.erase(zmq_url);
    }
}

// 发送数据到发布通道，供外部用户或服务订阅
int llm_channel_obj::send_raw_to_pub(const std::string &raw)
{
    return zmq_[-1]->send_data(raw);
}

// 发送数据到用户回传通道
int llm_channel_obj::send_raw_to_usr(const std::string &raw)
{
    if (zmq_[-2]) {
        return zmq_[-2]->send_data(raw);
    } else {
        return -1;
    }
}

// 设置用户回传通道地址，并创建对应的 PUSH 连接；如果地址有变更则重建连接
void llm_channel_obj::set_push_url(const std::string &url)
{
    if (output_url_ != url) {
        output_url_ = url;
        zmq_[-2].reset(new pzmq(output_url_, ZMQ_PUSH));
    }
}

// 清理用户回传通道，恢复默认状态
void llm_channel_obj::cear_push_url()
{
    zmq_[-2].reset();
}

// 静态工具函数：直接根据给定 URL 创建临时 ZMQ PUSH 连接并发送数据，适用于一次性发送场景
int llm_channel_obj::send_raw_for_url(const std::string &zmq_url, const std::string &raw)
{
    pzmq _zmq(zmq_url, ZMQ_PUSH);
    return _zmq.send_data(raw);
}
