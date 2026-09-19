#include "unit_data.h"
unit_data::unit_data()
{
}

// 初始化外部用户推理时，PUB发送数据的通信channel
void unit_data::init_zmq(const std::string &url)
{
    inference_url           = url;
    user_inference_chennal_ = std::make_unique<pzmq>(inference_url, ZMQ_PUB);
}

// 通过 PUB 通道发送推理消息给业务节点，供业务节点订阅处理
void unit_data::send_msg(const std::string &json_str)
{
    // pub-外部客户->业务节点
    user_inference_chennal_->send_data(json_str);
}

unit_data::~unit_data()
{
    user_inference_chennal_.reset();
}