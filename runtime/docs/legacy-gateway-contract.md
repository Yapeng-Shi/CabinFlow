# Legacy Gateway Contract Input

## 边界

本文只记录 `sample/test.py` 和 `node/test/src/main.cpp` 表现出的旧协议语义，供 Gateway 与 control contract test 使用。它们不接入 Runtime 顶层 CMake，也不构成新 Gateway 的兼容实现。

## 已表征的 TCP 语义

- 旧客户端把 UTF-8 JSON 文本追加 `\n` 后写入 TCP；接收端按换行拆分。因此它是应用层约定，不是 TCP 自带消息边界。
- 单连接响应读取只等待第一个换行；流式读取循环拆分多个非空行。新测试必须覆盖请求被拆分和多条响应被合并时的字节流，而不能把一次 `recv()` 当成一帧。
- 请求身份字段是 `request_id` 与 `work_id`。`setup` 用客户端请求 ID 关联响应，成功时返回可用于后续 `inference` 和 `exit` 的 work ID。
- `setup` 的旧动作字符串是 `action=setup`，并携带 model、input、response_format、enoutput、max_token_len、prompt 等配置；失败由 `error.code != 0` 表示。
- `inference` 使用 `action=inference`、`object=llm.utf-8.stream`，data 里包含 `delta`、`index`、`finish`。一个请求可以产生零到多条流式响应，只有 `data.finish=true` 表示该轮结束。
- `exit` 使用 request/work 身份和 `action=exit`，其旧节点实现会停止 task、停止订阅并移除任务对象；重复或未知 work ID 返回错误。
- `taskinfo` 的旧节点语义是查询所有任务或指定任务配置，但当前 Python 客户端未调用它。

## 迁移决策边界

新 Runtime 必须保留：请求关联、work 隔离、显式错误、流式增量与 final、取消/exit 后拒绝后续输入。

新 Runtime 不继承：任意 `action/object` 字符串分发、换行 JSON 作为生产协议、旧节点中的全局 stream buffer 与静态全局 delta 计数。

## 新 TCP 帧格式

Gateway 的唯一生产帧格式固定为：4 字节无符号大端 `body_size`，后接恰好 `body_size` 字节的 `RuntimeMessage` Protobuf 二进制。它不包含换行符、JSON、magic header、压缩或校验和。

第一版固定 `kFrameHeaderBytes = 4` 与 `kMaxFrameBytes = 4 * 1024 * 1024`。Reader 可以缓存拆开的长度头、拆开的帧体和一次读取中的多帧；只有完整帧体才调用 Protobuf 解码。零长度、超限和损坏 Protobuf 都是终态 framing error，连接拥有者必须关闭连接，不能尝试 JSON 或其他格式。

`gateway_framer_contract_test` 已验证上述分包、合包、长度前缀与终态错误。`ControlGateway` 已将该 Framer 接入唯一 TCP 控制入口：长度错误与 RuntimeMessage Protobuf 损坏直接关闭；可关联的 schema 错误会发送结构化错误后 half-close；连接断开只丢弃连接缓存，不自动取消 work。

## 类型化控制面

`control.proto` 固定 `ControlRequest` 和 `ControlResponse` 的 oneof 命令集。`RuntimeMessage.topic` 为 `control.request` 且 kind 为 Data 时，payload 唯一解码为 `ControlRequest`；响应 payload 唯一对应 `ControlResponse`，并固定使用 `control.response`。不根据 payload 内容猜测 JSON、命令字符串或其他格式。

控制请求不进入 `SessionLedger`。`ControlEnvelopeValidator` 按命令检查 session/work 身份：RegisterUnit 两者为空，Setup 只有 session，Pause/Exit 和 WORK TaskInfo 两者非空，SESSION TaskInfo 只有 session。Setup 由 `ControlService` 调用 `UnitRegistry` 原子检查 Unit、容量、生成并写入 work，随后在响应 body 返回 work ID；之后的数据面消息才带着该 ID 进入 `SessionLedger`。

`ControlService` 已实现 `(session_id, message_id)` 的 Setup 成功结果去重：相同内容返回首次 work ID，不同内容返回冲突。每次实际 TCP 写回仍由 Gateway 新建独立响应消息。

响应的 `message_id` 由 Gateway 生成，`ControlResponse.request_message_id` 固定关联原请求；trace 与 session 回显，source/target 交换，topic 为 `control.response`，成功为 DATA、错误为 ERROR，且 final 为 true。Setup 成功时 Envelope 的 work ID 必须与 `SetupResponse.work.work_id` 相同；失败为空。ResponseSequencer 对每个 `(session_id, work_id, control.response)` 流从 0 开始递增；只有无 session 的注册响应额外按连接 ID 隔离，连接 ID 不写入 Envelope。响应 TTL 是 Gateway 构造时明确提供的参数，没有默认值。
