import socket
import json
import argparse


def create_tcp_connection(host, port):
    # 创建 TCP 套接字对象
    # socket.AF_INET 表示使用 IPv4 地址族（比如 127.0.0.1、192.168.x.x）
    # socket.SOCK_STREAM 表示使用“流式”传输，也就是 TCP 协议（有连接、可靠、按序）
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    return sock


# 把 data 序列化成 JSON 字符串，并在末尾添加换行符（\n）作为消息分隔符，然后将其编码为 UTF-8 字节流发送到服务器
def send_json(sock, data):
    json_data = json.dumps(data, ensure_ascii=False) + "\n"
    sock.sendall(json_data.encode("utf-8"))


def receive_response(sock):
    response = ""
    while True:
        part = sock.recv(4096).decode("utf-8")
        response += part
        if "\n" in response:
            break
    return response.strip()


def close_connection(sock):
    if sock:
        sock.close()


def create_init_data():
    return {
        "request_id": "llm_001",  # 唯一标识一次请求，便于匹配响应
        "work_id": "llm",  # TASK ID，标识请求的处理对象或上下文
        "action": "setup",  # action 定义了请求的操作类型，setup 表示初始化或配置操作
        "object": "llm.setup",  # 目标对象，llm.setup 表示针对 LLM 的设置操作
        "data": {  # 请求的具体数据内容，包含模型选择、输入输出格式、最大 token 长度等配置项
            "model": "DeepSeek-R1-Distill-Qwen-1.5B",  # 使用的语言模型
            "response_format": "llm.utf-8.stream",  # 响应格式，指定为 UTF-8 编码的流式输出
            "input": "llm.utf-8.stream",  # 输入格式，指定为 UTF-8 编码的流式输入
            "enoutput": True,  # 是否与用户回传通道交互，true 表示启用回传通道
            "max_token_len": 1023,  # 模型生成的最大 token 长度，限制输出长度以控制响应时间和资源使用
            "prompt": "You are a knowledgeable assistant capable of answering various questions and providing information.",  # 模型的提示词，指导模型生成符合预期的回答
        },
    }


# 解析 setup 响应，检查请求 ID 是否匹配，处理错误信息，并返回 work_id 以供后续请求使用
def parse_setup_response(response_data, sent_request_id):
    error = response_data.get("error")
    request_id = response_data.get("request_id")

    if request_id != sent_request_id:
        print(f"Request ID mismatch: sent {sent_request_id}, received {request_id}")
        return None

    if error and error.get("code") != 0:
        print(f"Error Code: {error['code']}, Message: {error['message']}")
        return None

    return response_data.get("work_id")


def setup(sock, init_data):
    sent_request_id = init_data["request_id"]
    send_json(sock, init_data)
    response = receive_response(sock)
    response_data = json.loads(response)
    return parse_setup_response(response_data, sent_request_id)


def exit_session(sock, deinit_data):
    send_json(sock, deinit_data)
    response = receive_response(sock)
    response_data = json.loads(response)
    print("Exit Response:", response_data)


def parse_inference_response(response_data):
    error = response_data.get("error")
    if error and error.get("code") != 0:
        print(f"Error Code: {error['code']}, Message: {error['message']}")
        return None

    return response_data.get("data")


def receive_json_stream(sock):
    """生成器函数，持续返回完整JSON对象"""
    buffer = ""
    while True:
        chunk = sock.recv(4096).decode("utf-8", errors="ignore")
        if not chunk:
            break
        buffer += chunk

        while "\n" in buffer:
            line, buffer = buffer.split("\n", 1)
            if line.strip():
                yield line.strip()


def main(host, port):
    sock = create_tcp_connection(host, port)

    try:
        print("Setup LLM...")
        init_data = create_init_data()
        llm_work_id = setup(sock, init_data)
        print("Setup LLM finished.")

        while True:
            user_input = input("Enter your message (or 'exit' to quit): ")
            if user_input.lower() == "exit":
                break

            # 流式和非流式输入，可以通过流式配置传输
            send_json(
                sock,
                {
                    "request_id": "llm_001",  # 唯一标识一次请求，便于匹配响应
                    "work_id": llm_work_id,  # unit_manager 分配的工作 ID，标识请求的处理对象或上下文
                    "action": "inference",  # action 定义了请求的操作类型，inference 表示推理或生成操作
                    "object": "llm.utf-8.stream",  # 目标对象，llm.utf-8.stream 表示针对 LLM 的 UTF-8 编码流式输入输出操作
                    "data": {
                        "delta": user_input,  # 输入内容，作为模型生成的上下文或提示词的一部分
                        "index": 0,  # 输入的索引或顺序，便于模型处理多轮对话或分段输入
                        "finish": True,  # 是否为输入的最后一部分，true 表示输入完成，模型可以开始生成响应；false 表示输入未完成，模型应等待更多输入
                    },
                },
            )

            while True:
                for raw_response in receive_json_stream(sock):

                    response = json.loads(raw_response)
                    print(f"all response: {response}")
                    delta = response.get("data", {}).get("delta", "")
                    finish = response.get("data", {}).get("finish", False)

                    if delta:
                        print(delta, end="", flush=True)
                        print()

                    if finish:
                        print()
                        break

        exit_session(
            sock, {"request_id": "llm_exit", "work_id": llm_work_id, "action": "exit"}
        )
    finally:
        close_connection(sock)


if __name__ == "__main__":
    # 创建命令行参数解析器，允许用户指定服务器的主机名和端口号，默认值分别为 localhost 和 10001
    parser = argparse.ArgumentParser(description="TCP Client to send JSON data.")
    parser.add_argument(
        "--host",
        type=str,
        default="localhost",
        help="Server hostname (default: localhost)",
    )
    parser.add_argument(
        "--port", type=int, default=10001, help="Server port (default: 10001)"
    )

    args = parser.parse_args()
    main(args.host, args.port)
