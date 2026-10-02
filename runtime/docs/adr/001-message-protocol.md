# ADR-001：版本化消息、身份与状态边界

## 状态

`CabinFlow::Protocol` 的 C++ Envelope、`RuntimeMessage` Protobuf 和 codec 已实现。
以下表格以当前 x86/WSL 代码为准；尚未实现的规则明确标为待决，不能当成已验证行为。

## 唯一编码与消息类别

TCP 只接收 4 字节大端长度前缀加 `RuntimeMessage` Protobuf，帧体上限 4 MiB；
非法长度或损坏的 Protobuf 直接断连。不提供旧换行 JSON、格式探测或回退入口。
`payload` 的唯一类型由明确的 topic 决定，Gateway 不解析 Agent 业务 payload。

| 语义 | 当前表示 | 准入与结束边界 |
|---|---|---|
| 控制 request | `topic=control.request`、`kind=DATA`、`ControlRequest.oneof` | `ControlEnvelopeValidator → ControlService`，不进入 `SessionLedger`；`Setup` 由 `UnitRegistry` 创建 work。 |
| 数据 partial | 业务 topic、`kind=DATA`、`is_final=false` | 预留目标队列后由 `SessionLedger` 准入；不产生成功 ACK。 |
| 数据 final | 同一业务 topic、`kind=DATA`、`is_final=true` | 准入后关闭该 `(session_id, work_id, topic)` 输入流。 |
| 结果与错误 | `control.response`、`runtime.delivery.error` 或 Agent 输出 topic | 响应使用新 `message_id`、独立序号；错误为 `kind=ERROR`，结果为 `DATA`，均不再进入输入 Ledger。 |
| cancel | wire `kind=CANCEL` 已定义，但 Gateway 不接受其作为取消入口 | 当前仅显式控制 `Exit` 成功后调用 Runtime work 取消；不把收到的 `CANCEL` 自动解释为权限命令。 |

## 字段所有权与失效边界

| 字段 | 谁提供或生成 | 作用域、校验与失效 |
|---|---|---|
| `schema_version` | 发送方填写；Gateway 响应写当前版本 | 当前版本为 1。不支持版本返回结构化错误并关闭连接；损坏的帧无法取得可信身份时直接断连。 |
| `trace_id` | TCP 客户端提供；Gateway 响应回显 | 串联同一业务链路的日志；当前不强制唯一，也不是授权凭据。 |
| `session_id` | 客户端提供；Gateway 响应回显 | 乘员会话边界。`RegisterUnit` 允许空；`Setup` 和数据请求必须非空。Runtime session 取消后，本进程内不能靠同一 ID 恢复准入。 |
| `work_id` | `ControlService/UnitRegistry` 在 `Setup` 成功时生成并返回；后续由客户端携带 | `Setup` 前为空；数据面必须非空，Gateway 核验其存在且属于该 session。成功 `Exit` 后不再准入该 work；Registry 保留已退出记录，ID 不复用。 |
| `message_id` | 客户端为每条入站消息提供；Gateway 为每条出站消息生成新 ID | 一条消息的身份，不等于 trace 或 work。`SessionLedger` 在本进程内对数据消息全局去重；重复 `Setup` 由 `(session_id, message_id)` 幂等键处理。Ledger 目前不淘汰已观察 ID，重启后内存状态消失。 |
| `sequence` | 客户端为入站流分配；Gateway 为每个响应流独立分配 | 输入在同一 `(session_id, work_id, topic)` 内严格递增，首值不强制为 0；响应流从 0 开始，不能回显请求序号。进程重启后响应序号可重新从 0 开始。 |
| `source_node` / `target_node` | 请求发送方填写；Gateway 响应交换方向 | 非空；数据的 target 由 Runtime 按唯一节点名查找。`target_node` 与 work 所属 `unit_id` 是否必须相等尚未验收。 |
| `topic` / `kind` / `is_final` | 发送方按唯一协议填写；Gateway 生成响应值 | topic 决定 payload 类型；`is_final` 只关闭进入 Ledger 的业务输入流，不能拿控制响应的 final 推断 work 已退出。 |
| `seat` | 客户端填写；Gateway 数据输出回显 | 区域元数据，当前不构成区域权限校验。 |
| `created_monotonic_ns` / `ttl_ms` | 请求发送方填写；Gateway 为响应生成创建时间和明确 TTL | 首版限定 TCP 客户端与 Gateway 共用同一主机的单调时钟域。控制请求在进入 `ControlService` 前、数据消息在准入/执行边界按 `now - created >= ttl_ms` 判过期；零 TTL、未来时间戳和过期均显式拒绝。跨主机单调时钟不可直接比较。 |

这里的“同机”指相同的操作系统单调时钟域，不能仅凭物理机器相同就假定时间戳可比较。
控制请求先校验期限，再进入 `ControlService` 去重：过期重发即使使用原 `message_id`，
也不会再次执行或返回之前的 Setup 成功结果；期限内的相同 Setup 才复用原 `work_id`。

## `SessionLedger` 输入状态转移

| 原状态与事件 | 结果 | 新状态 |
|---|---|---|
| 未见流 + 合法 non-final | `ACCEPTED` | Open，记下 message ID 和 sequence。 |
| 未见流或 Open + 合法 final、序号递增 | `ACCEPTED` | Finalized，后续同流输入拒绝。 |
| Open + 合法 non-final、序号递增 | `ACCEPTED` | Open，更新 sequence。 |
| 任意状态 + 缺失身份、未来时间戳、零 TTL、过期或不支持 schema | 显式拒绝 | 不修改 ID、sequence 或 final 状态。 |
| 任意状态 + 已观察的 `message_id` | `DUPLICATE_MESSAGE` | 不变。 |
| Open + 不递增的 sequence | `STALE_SEQUENCE` | 不变。 |
| Finalized + 新 ID 的后续输入 | `STREAM_FINALIZED` | 不变。 |
| 任意状态 + work/session 取消 | 后续分别为 `WORK_CANCELLED` / `SESSION_CANCELLED` | 取消状态单调保持到 Runtime 销毁；已排队但未开始的消息由 worker 跳过，正在执行的副作用不假装回滚。 |

Ledger 的检查顺序是 schema/身份/时间 → session/work 取消 → 重复 ID →
final → 序号。控制 work 的 `RUNNING/PAUSED/EXITED` 属于 `UnitRegistry`，不是
Ledger 的流状态；`Pause` 后数据准入规则尚未确定。

## 可重复测试矩阵

| 类别 | 预期 | 对应测试 |
|---|---|---|
| 必填身份为空、零 TTL、未来时间戳、恰好到期、错误 schema | 各自显式拒绝；修正同一 ID/序号后可准入 | `session_ledger_test` 的表驱动矩阵。 |
| 重复 ID、同流乱序、final 后续输入 | 分别拒绝，原流状态不被错误输入推进 | `session_ledger_test`。 |
| work/session 取消与其他 session 隔离 | 取消目标拒绝；其他范围仍可准入 | `session_ledger_test`、`cancellation_test`、`target_runtime_contract_test`。 |
| `Setup` 空 work、命令身份、控制响应关联 | 控制面处理，不放宽数据 Ledger 的非空 work 规则 | `control_plane_contract_test`、`control_gateway_tcp_test`。 |
| TCP 半包/粘包、损坏帧、数据错误 | 完整帧才解码；不可关联则断连，可关联则类型化错误 | `gateway_framer_contract_test`、`data_plane_gateway_tcp_test`。 |
| 控制请求恰好到期、未到期与未来时间戳 | 过期和未来时间戳返回关联错误；过期 `Setup` 不创建 work，连接可继续使用 | `control_plane_contract_test`、`control_gateway_tcp_test`。 |

## 尚未完成的边界

跨主机时间域方案、`Pause` 数据规则，及
`target_node` 与 `unit_id` 一致性仍需明确决策和测试；不能用已有数据面测试
推断这些行为已实现。
