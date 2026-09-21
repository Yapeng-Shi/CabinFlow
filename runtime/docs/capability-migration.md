# CabinFlow Runtime 能力迁移账本

## 目的与边界

本账本以可验证的能力为迁移单位，而非以历史目录为迁移单位。`infra-controller/`、`unit-manager/`、`network/` 与 `hybrid-comm/` 是当前迁移输入材料：它们不接入顶层 CMake，也不能被当作新 Runtime 已经实现过的证据。

旧实现只能在对应能力达到 `verified`，或用户明确批准该能力为 `approved-drop` 后删除。已列入迁移账本的旧模块和样例输入均已恢复；`runtime/node/llm/README.md` 与旧 in-memory transport 测试按已记录决策保持删除且未暂存。

状态只使用以下值：

| 状态 | 含义 |
|---|---|
| `pending` | 尚未建立行为测试或开始迁移。 |
| `characterized` | 旧行为已被自动化测试固定。 |
| `migrating` | 新实现正在建设，旧实现仍保留作对照。 |
| `verified` | 新实现通过对应测试，调用方已经完成切换。 |
| `approved-drop` | 用户明确同意裁剪，已记录原因。 |

## 当前能力清单

| 能力 | 旧入口 | 调用方 | 外部依赖 | 决策 | 新位置 | 测试 | 状态 |
|---|---|---|---|---|---|---|---|
| epoll Reactor 与跨线程唤醒 | `network/EventLoop`、`Poller`、`Channel` | `TcpServer`、`TcpClient` | Linux epoll、eventfd | 迁移，先保持行为 | `net/` | legacy characterization；新 `event_loop_test` 的 Debug/ASan/TSan | migrating |
| 字节缓冲、地址与 socket 基元 | `Buffer`、`InetAddress`、`Socket`、`SocketsOps` | TCP server/client | POSIX socket | 迁移，作为 Reactor 基础 | `net/` | legacy characterization；新 Buffer/InetAddress/Socket 单测的 Debug/ASan/TSan | migrating |
| TCP 服务端与连接生命周期 | `TcpServer`、`TcpConnection`、`Acceptor` | `unit-manager/src/tcp_comm.cpp` | epoll、pthread | 迁移 | `net/` | legacy characterization；新 TcpConnection peer close/回调内关闭/响应，TcpServer 双连接、2 worker、停止释放，Debug/ASan/TSan | migrating |
| TCP 客户端与连接器 | `TcpClient`、`Connector` | 历史 network API 使用方 | epoll、timerfd | 迁移；单次非阻塞连接，失败显式回调，不虚构旧代码未启用的 timer 重试 | `net/` | TcpClient 成功字节流、失败 `ECONNREFUSED`、连接回调内 stop；Debug/ASan/TSan | migrating |
| EventLoop 线程与线程池 | `EventLoopThread`、`EventLoopThreadPool` | `TcpServer` | pthread | 迁移 | `net/` | worker 初始化、零 worker 使用 base loop、轮询分配、跨线程投递；Debug/ASan/TSan | migrating |
| 旧依赖安装记录 | `runtime/build.sh` | 历史独立构建流程 | eventpp、simdjson、ZeroMQ | 保留为依赖迁移输入，不是当前构建入口 | `docs/` | 依赖清单与新 CMake 依赖对照 | pending |
| TCP 帧解析与断连语义 | `TcpSession::select_json_str`、`onMessage` | `unit-manager` TCP 接入 | JSON | 重做为 4 字节大端长度前缀加 `RuntimeMessage` Protobuf，不继承 JSON 字符串协议 | `gateway/` | Framer contract；`control_gateway_tcp_test`：分段帧、结构化 schema 错误后关闭、损坏 Protobuf 直接关闭 | migrating |
| setup/pause/exit/taskinfo 控制命令 | `StackFlow::_rpc_*` 与虚函数 | 各历史业务单元 | pzmq、JSON、eventpp | 重做为类型化 Protobuf oneof 命令 | `protocol/control.proto`、`gateway/` | `control_plane_contract_test`：命令身份、scope、未知命令、reason、重复 setup、无效 work | migrating |
| 节点注册与状态查询 | `sys_register_unit`、`sys_allocate_unit` | `StackFlow::setup`、remote server | pzmq、全局内存状态 | 重做，所有权归 Registry | `core/unit_registry.hpp` | `control_plane_contract_test`：注册、容量、状态转换、查询 | migrating |
| work 生命周期结束 | `StackFlow::exit`、`sys_release_unit` | 历史业务单元 | pzmq | ControlService 负责命令，Registry 原子转换并释放容量 | `core/`、`gateway/` | `control_plane_contract_test`：pause、exit、exit 后拒绝后续命令 | migrating |
| session/work 取消 | 历史 `exit` 仅释放单元，不具备独立取消语义 | 新 Runtime 节点 | 无 | 新建语义，保留为 Runtime Core | `core/cancellation.*`、`session_ledger.*` | `cancellation_flow_test` | migrating |
| 消息准入、去重、顺序、final 状态 | 历史 ZMQ JSON 数据流无统一 admission gate | 新 Runtime 节点 | 无 | 新建语义，保留为 Runtime Core | `core/session_ledger.*` | core 单测、two-node flow | migrating |
| 节点数据平面 PUB/SUB | `hybrid-comm/pzmq`、`llm_channel_obj` | `StackFlow`、`unit-manager` | ZeroMQ | 重做，屏蔽 ZeroMQ 头文件 | `transport/zmq/` | transport 单测、端到端 ZMQ 测试 | migrating |
| TCP 到 Runtime 的消息桥接 | `TcpSession`、`zmq_bus_com`、`unit_action_match` | `unit-manager` | TCP、ZMQ、JSON | 控制面已重做为唯一 Gateway；数据面尚未接入 SessionLedger/queue/target node | `gateway/` | `control_gateway_tcp_test` 已覆盖 TCP 到 ControlService；数据面 MessageEnvelope 集成测试待建 | migrating |
| 按 work_id 路由 | `llm_channel_obj`、`unit_data` | `StackFlow`、`zmq_bus` | ZeroMQ endpoint | 重做，优先 topic 与统一 Transport | `core/`、`transport/` | work 隔离、错误路由、取消后拒绝 | pending |
| 控制命令、流式响应与 work_id 旧语义 | `node/test/src/main.cpp` | 历史测试节点 | StackFlow、pzmq | 保留为 characterization 输入 | `control/`、`gateway/`、`transport/` | 逐项转写为 contract test | pending |
| TCP JSON、流式响应与 setup/exit/inference 协议 | `sample/test.py` | 历史 TCP 客户端 | TCP、JSON | 保留为 Gateway 与 control contract 输入 | `gateway/`、`control/` | frame/response contract test | pending |
| TCP 多连接压力场景 | `sample/stress.py` | 历史 TCP 客户端 | TCP、Python threading | 保留为行为参考；共享计数竞态不作为性能指标 | `net/`、`gateway/` | 多连接正确性测试；独立基准工具 | pending |
| PUB/SUB 与 RPC 实际调用样例 | `sample/pub.cc`、`sub.cc`、`rpc_call.cc`、`rpc_server.cc`、`pz_rpc_call.cc`、`pz_rpc_server.cc` | 历史手工样例 | ZeroMQ、pzmq | 保留为 ZMQ 行为参考 | `transport/zmq/`、`control/` | ZMQ transport 与 control contract test | pending |
| 旧 in-memory transport 测试 | `tests/integration/in_memory_transport_test.cpp` | CTest | 无 | 已直接迁移；旧路径不恢复 | `tests/unit/transport/in_memory_transport_test.cpp` | `in_memory_transport_test`，已纳入当前 CTest | verified |
| 运行进程装配与信号退出 | `unit-manager/src/main.cpp` | 独立进程 | POSIX signal、JSON 配置 | 重做 | `apps/runtime_daemon/` | 启动配置校验、SIGTERM 逆序退出 | pending |
| 全局任意键值存储 | `key_sql`、`sys_sql_*` | remote server、StackFlow | 全局内存、JSON | 候选裁剪：无明确所有者且不满足 Registry 语义 | 无 | 不适用；等待用户批准裁剪 | pending |
| 任意 remote action 字符串分发 | `remote_call`、`unit_action_match` | unit-manager | JSON、动态 action | 候选裁剪：仅保留类型化控制命令 | 无 | 不适用；等待用户批准裁剪 | pending |
| 每个任务动态分配 ZMQ 端口 | `sys_allocate_unit` 的 port list | remote server、unit_data | ZeroMQ、端口池 | 候选裁剪：默认使用统一 endpoint/topic | 无 | 不适用；只有证明约束后才恢复 | pending |

## 已恢复的 characterization 输入

以下路径只作为旧行为、旧协议和历史依赖的取证输入；它们不通过顶层 CMake 构建，也不是新 Runtime 的回退路径：

- `runtime/build.sh`；
- `runtime/utils/json.hpp` 与 `runtime/utils/sample_log.h`；
- `runtime/node/test/`；
- `runtime/sample/`；
- `infra-controller/`、`unit-manager/`、`network/` 与 `hybrid-comm/`。

`runtime/node/llm/README.md` 未恢复，其删除已取消暂存，等待在账本中获得明确的 `approved-drop` 后再删除。

## 网络底座迁移进度

旧 `network/tests/` 不接入顶层构建，继续作为行为对照。它已经覆盖 Buffer 字节序、EventLoop 跨线程唤醒、EventLoopThreadPool worker 启动与轮询分配，以及 TcpServer 的分段/合并字节流、多连接、peer 主动断开、callback 内关闭和服务端停止释放。

新 `CabinFlow::Net` 已迁移 `EventLoop / Channel / Poller`、内部 `Buffer / Socket / InetAddress / Acceptor / Connector / EventLoopThread / EventLoopThreadPool`，以及公开 `TcpConnection / TcpServer / TcpClient`。TcpServer 以 `set_worker_count()` 在启动前选择 worker 数；`stop()` 先拒绝新连接，等待各 worker 的 close 回调擦除连接，再 join worker。TcpClient 是单次非阻塞连接：成功后把 FD 所有权交给 TcpConnection，失败以 `std::error_code` 回调报告，不含隐藏重试。当前新 Runtime 在 WSL/x86 的 Debug 20/20、ASan 20/20 下通过，网络相关 TSan 8/8 通过；两 worker 的 TcpServer 场景额外重复 Debug 20 次和 TSan 10 次均通过。TSan 必须用 `setarch x86_64 -R` 避免初始化地址映射失败。旧测试的 TSan 只抑制 `google::LogMessageTime::CalcGmtOffset` 的第三方 glog 时区竞态；不抑制任何 `network` 报告。Gateway 已开始唯一 Protobuf 帧路径：`RuntimeMessageFramer` 以 4 字节大端长度前缀缓存 TCP 字节流，只在收齐完整帧时解码；长度与 Protobuf 错误进入终态，由未来 TCP 连接拥有者关闭。它尚未接入 TCP Server、ControlService 或 Runtime。

## 当前明确禁止

- 不提交或再次暂存任何尚未迁移能力的删除。
- 不把 `net/`、`control/`、`gateway/` 创建为空目录占位。
- 不保留新旧接口双路径作为运行时回退。
- 不把 WSL/x86 测试写成 RK3576 或车端部署验证。
