# CabinFlow Runtime 目录结构设计

更新日期：2026-10-02。本文保留现有目录职责、公开 target 和依赖，不代表所有旧能力已迁移；当前开发优先级已收敛为可交互 Demo。

## 当前优先级：复用 Runtime，先完成 Demo

当前执行入口是[Demo 实施计划](IMPLEMENTATION_PLAN.md)。用户已批准不再先做完整通用 Runtime/历史迁移收尾；只修 Demo 实际使用的安全退出、准入、取消和结果关联缺口，随后冻结 Runtime 扩展。

- 真实 ASR/LLM/TTS、Qt/QML 界面、回答播放和小型 FakeVehicle 演示归 `agent/`，不进入 Runtime 核心。
- 首版只需要一个后端进程和同 WSL Linux 下的 Qt 前端，复用现有 TCP/Protobuf 接入；后端内部复用队列，不为使用 ZeroMQ 拆多进程。
- 只补 Demo 必需的 Agent 应用装配，不调整现有 Runtime 目录，不预先定义大量新文件/target。
- 2026-10-02 同日补充批准：系统集成/组件解耦优先，模型质量不再卡住装配。Agent 模型端口和具体适配器分离，启动时构造注入；不建设插件/工厂注册或自动回退。
- Runtime 原构建入口不变。真实产品的显式装配入口位于 `agent/apps/voice_demo/`，用独立 build 目录复用 Runtime targets，并私有链接模型适配器；默认 Runtime 构建不要求模型 SDK。这是应用装配，不是第二个 Runtime 实现。
- 正式 `runtime_daemon`、全量旧样例迁移、插件、重启、异步 stop、跨平台 IPC 和长期资源治理后置；这些项目不使用、不标完成，也不作为短时单任务 Demo 的总门槛。
- 旧能力与源码仍受第 6 节删除门槛约束；冻结扩展和调整投入不授权删除。

## 文档分工与执行规则

- 本文：模块放在哪里、由谁拥有、允许依赖谁。
- [实施计划](IMPLEMENTATION_PLAN.md)：阶段顺序、当前进度、待决事项和验收门槛。
- [消息协议 ADR](../runtime/docs/adr/001-message-protocol.md)及其他相关 ADR：已确认的协议、状态和线程语义。
- [能力迁移账本](../runtime/docs/capability-migration.md)：每项旧能力的保留决策、测试证据和删除许可。
- [第一周验收](../runtime/docs/week1-acceptance.md)及 Agent 里程碑记录：按日期保存实际环境、命令、结果和未验证边界。

以上文档各自管理对应职责，不按文件新旧自动覆盖其他合同。发现冲突时先暂停受影响项；已批准但未实现的规则与真正待决事项分开记录，并在实现前同步相关 ADR。修改计划不等于实现或验收通过。

## 1. 设计目标

Runtime 负责座舱 Agent 的通用运行基础设施：消息契约、传输边界、节点生命周期、会话与任务隔离、取消、背压和可观测性。它不包含 ASR、RAG、LLM、TTS、对话策略或车辆业务。

生产库目录按稳定职责拆分，各自对应独立 CMake target；`apps/`、`tests/`、`docs/` 和 `scripts/` 不要求各自形成库。这样能够在编译期限制依赖方向，也能让单元测试只链接被测模块。

## 2. 当前规范结构

以下是现有活动构建的职责索引，列出关键文件而非全部文件；省略号不表示待创建的占位文件。目录存在不等于该目录承接的历史能力全部达到 `verified`。

```text
runtime/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   ├── CompilerWarnings.cmake
│   └── Sanitizers.cmake
│
├── protocol/
│   ├── CMakeLists.txt
│   ├── proto/
│   │   ├── runtime_envelope.proto
│   │   ├── control.proto
│   │   └── delivery.proto
│   ├── include/cabinflow/protocol/
│   │   ├── message.hpp
│   │   ├── message_codec.hpp
│   │   ├── message_envelope.hpp
│   │   ├── message_kind.hpp
│   │   └── protocol_error.hpp
│   └── src/
│       └── message_codec.cpp
│
├── transport/
│   ├── CMakeLists.txt
│   ├── include/cabinflow/transport/
│   │   ├── transport.hpp
│   │   ├── subscription.hpp
│   │   └── transport_error.hpp
│   ├── in_memory/
│   │   ├── CMakeLists.txt
│   │   ├── include/cabinflow/transport/in_memory/
│   │   │   └── in_memory_transport.hpp
│   │   └── src/
│   │       └── in_memory_transport.cpp
│   └── zmq/
│       ├── CMakeLists.txt
│       ├── include/cabinflow/transport/zmq/
│       │   └── zmq_transport.hpp
│       └── src/
│           └── zmq_transport.cpp
│
├── net/
│   ├── CMakeLists.txt
│   ├── include/cabinflow/net/
│   │   ├── event_loop.hpp
│   │   ├── tcp_connection.hpp
│   │   ├── tcp_server.hpp
│   │   └── tcp_client.hpp
│   └── src/
│       ├── event_loop.cpp
│       ├── tcp_connection.cpp
│       ├── tcp_server.cpp
│       ├── tcp_client.cpp
│       └── …                       # Reactor、Socket、Buffer、连接器与线程池内部实现
│
├── core/
│   ├── CMakeLists.txt
│   ├── include/cabinflow/runtime/
│   │   ├── bounded_queue.hpp
│   │   ├── cancellation.hpp
│   │   ├── clock.hpp
│   │   ├── node.hpp
│   │   ├── node_context.hpp
│   │   ├── runtime.hpp
│   │   ├── runtime_error.hpp
│   │   ├── session_ledger.hpp
│   │   ├── target_node.hpp
│   │   ├── unit_registry.hpp
│   │   └── version.hpp
│   └── src/
│       ├── cancellation.cpp
│       ├── clock.cpp
│       ├── node.cpp
│       ├── runtime.cpp
│       ├── session_ledger.cpp
│       └── unit_registry.cpp
│
├── gateway/
│   ├── CMakeLists.txt
│   ├── include/cabinflow/gateway/
│   │   ├── runtime_message_framer.hpp
│   │   ├── control_envelope_validator.hpp
│   │   ├── data_envelope_validator.hpp
│   │   ├── control_service.hpp
│   │   ├── control_gateway.hpp
│   │   ├── control_response_builder.hpp
│   │   ├── delivery_error_builder.hpp
│   │   └── response_sequencer.hpp
│   └── src/
│       ├── runtime_message_framer.cpp
│       ├── control_service.cpp
│       ├── control_gateway.cpp
│       └── …                       # 校验、响应构建及消息身份生成
│
├── observability/
│   ├── CMakeLists.txt
│   ├── include/cabinflow/observability/
│   │   ├── event.hpp
│   │   ├── logger.hpp
│   │   └── metrics.hpp
│   └── src/
│       ├── logger.cpp
│       └── metrics.cpp
│
├── apps/
│   └── runtime_demo/
│       ├── CMakeLists.txt
│       ├── main.cpp
│       ├── demo_nodes.hpp
│       ├── text_source_node.cpp
│       └── echo_processor_node.cpp
│
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── protocol/
│   │   ├── transport/
│   │   ├── net/
│   │   ├── core/
│   │   └── observability/
│   ├── contract/
│   │   ├── gateway_framer_contract_test.cpp
│   │   ├── control_plane_contract_test.cpp
│   │   ├── delivery_error_contract_test.cpp
│   │   └── target_runtime_contract_test.cpp
│   ├── integration/
│   │   ├── two_node_flow_test.cpp
│   │   ├── cancellation_flow_test.cpp
│   │   ├── zmq_transport_test.cpp
│   │   ├── control_gateway_tcp_test.cpp
│   │   ├── data_plane_gateway_tcp_test.cpp
│   │   ├── runtime_demo_output.cmake
│   │   └── fake_voice_demo_output.cmake
│   ├── stress/
│   │   └── bounded_queue_stress_test.cpp
│   └── support/
│       ├── fake_clock.hpp
│       ├── message_builder.hpp
│       └── recording_logger.hpp
│
├── docs/
│   ├── architecture.md
│   ├── development.md
│   ├── capability-migration.md
│   ├── legacy-gateway-contract.md
│   ├── week1-acceptance.md
│   ├── sanitizers.md
│   └── adr/
│
└── scripts/
    ├── build.sh
    └── test.sh
```

业务边界位于同级 `agent/`：已有 `agent/protocol/proto/cockpit_text.proto`、`cockpit_audio.proto`、`agent/dialogue/` 和 `agent/apps/fake_voice_demo/`。业务 payload 不因被 Runtime 构建装配而迁入 `runtime/protocol/`。

`apps/runtime_daemon/` 是能力账本中尚未开始的进程装配目标，不是当前完成项；开始对应实现前不创建目录。已恢复的历史源码另见第 6 节，不出现在活动模块索引中，也不参与根构建。

## 3. 各层职责

### `protocol/`

拥有跨节点传递的数据契约和编解码：

- `MessageEnvelope` 及消息类型；
- `trace_id/session_id/work_id/message_id/sequence`；
- deadline、final、error、cancel 等协议字段；
- Protobuf schema 与 C++ domain type 的转换；
- schema version 校验。

`control.proto` 定义控制命令与响应，`delivery.proto` 定义 Runtime 投递错误；ASR、文本、音频、Tool Call 等业务 payload 归 `agent/protocol/`。不负责消息是否应该被业务处理，也不维护 session 状态。

### `transport/`

拥有消息“如何送达”的抽象和具体实现：

- `ITransport` 接口；
- subscription 生命周期；
- transport 层错误；
- 进程内确定性实现；
- ZeroMQ 实现及其 I/O 线程所有权。

业务代码只能依赖 `cabinflow_transport_api`，不能包含 ZeroMQ 头文件。`in_memory` 用于测试和本地 demo，不是 ZeroMQ 失败后的运行时回退。

### `net/`

拥有 Linux TCP 字节流基础设施：Reactor、事件循环、socket、连接生命周期、服务端/客户端及 EventLoop 线程池。内部 Buffer、Poller、Channel、Connector 等放在 `src/`，仅稳定的 EventLoop 与 TCP 接口公开。

`net/` 不知道 Runtime Envelope、Protobuf、work 或业务 topic。TCP 字节流不自行识别消息边界；帧和控制命令属于 `gateway/`。它不是第二套 `ITransport`，也不是旧 `network/` API 的兼容包装。

### `core/`

拥有消息进入节点后的运行语义：

- `SessionLedger` admission decision；
- bounded queue 和背压；
- work/session 取消；
- node lifecycle；
- callback 调度和对象生命周期；
- deadline 检查；
- Runtime 启停和资源释放顺序。

`TargetNode::on_message(const protocol::Message&)` 是现有通用数据入口，队列与 worker 由 Runtime 拥有；Agent 节点解析业务 payload。`UnitRegistry` 拥有 Unit 容量与 work 状态，不负责 socket、帧或控制响应构建。

`core/` 不知道 socket 帧、ZeroMQ endpoint 或 Agent 的业务 payload。每 Target 队列有界不等于全进程内存有界；Ledger、Registry、取消历史及输出积压需要各自的保留/容量合同，不可通过悄悄清理状态改变去重或取消语义。

### `gateway/`

拥有外部 TCP 与 Runtime 的接入边界：

- 唯一 `4B BE length + RuntimeMessage Protobuf` 分帧，上限 4 MiB；不接收 JSON，不做格式探测。
- 控制面：`ControlEnvelopeValidator → ControlService → UnitRegistry`；Setup 创建 work，不进入要求非空 work 的数据 Ledger。
- 数据面：Envelope/work 校验 → 目标队列预留 → Ledger 准入 → worker/TargetNode；队列满不消耗消息 ID、序号或 final 状态。
- 类型化控制响应和 `runtime.delivery.error`；输出按输入身份关联回原 TCP 连接，断连不自动取消 work。

Gateway 不链接 AgentProtocol，也不解析业务 payload。本轮只按实施计划解决 Demo 实际使用的非法业务 final 校验/提交边界，以及到最终结果的关联存活期；通用异步输出和 Pause 数据规则后置，不在首版使用，也不能从当前同步文本测试推断这些能力已完成。

### `observability/`

拥有统一结构化事件和指标接口：

- 日志字段规范；
- queue depth、rejection count、handler latency 等指标；
- trace/session/work/message 身份关联；
- 测试可替换的 logger/metric sink。

第一版只实现项目真正使用的结构化日志和内存指标，不提前接入大型监控框架。

### `apps/`

只放可运行程序和依赖装配：

- 选择具体 transport；
- 构造 Runtime 和节点；
- 解析启动配置；
- 处理进程信号和退出。

`main.cpp` 不实现队列、协议或业务算法。`runtime_demo` 验证通用双节点语义；`agent/apps/fake_voice_demo/` 验证业务消息链，两者职责不同。前者不承接真实语音，后者不承接模型失败回退。

新的真实语音 Demo 后端及 Qt/QML 前端装配属于 Agent 应用；它们只消费公开 Runtime/Gateway 接口。Qt 依赖留在前端，模型依赖留在 Agent 节点，Runtime 核心不依赖 Qt、音频播放或 FakeVehicle。先做能运行的最小装配，不为 Demo 额外建设正式 daemon、配置中心或应用管理框架；Runtime daemon 仍是账本后置项。

### `tests/`

- `unit/`：单模块、不启动真实外部服务；
- `contract/`：帧、控制请求、投递错误和目标准入等公开合同；
- `integration/`：多个 Runtime 组件之间的数据流；
- `stress/`：并发、容量和竞态边界，不作为普通单元测试运行时间的负担；
- `support/`：仅测试使用的 fake 和 builder，生产 target 不得链接。

## 4. CMake target 与依赖方向

现有 Runtime 公开 target：

```text
CabinFlow::Protocol
CabinFlow::TransportApi
CabinFlow::TransportInMemory
CabinFlow::TransportZmq
CabinFlow::Observability
CabinFlow::Net
CabinFlow::Runtime
CabinFlow::Gateway
```

以下箭头统一表示“左侧依赖右侧”，以当前 CMake 链接关系为准：

```text
TransportApi      -> Protocol
TransportInMemory -> TransportApi
TransportZmq      -> TransportApi (+ 私有 libzmq/Threads)
Runtime           -> Protocol + TransportApi + Observability
Gateway           -> Protocol + Runtime + Net

AgentProtocol     -> Protobuf（业务 schema，不依赖 Runtime）
DialogueTextNode  -> Runtime (+ 私有 AgentProtocol)
apps/tests        -> 所需公开 target 与具体 transport
```

当前构建入口是 `runtime/CMakeLists.txt`，它还装配已有 `CabinFlow::AgentProtocol`、`CabinFlow::DialogueTextNode` 和 `fake_voice_demo`。构建装配不是库依赖；`Runtime` 和 `Gateway` 不因此反向依赖 Agent。内部真实 target 如 `cabinflow_runtime` 与公开 alias `CabinFlow::Runtime` 是同一库的 CMake 名称，不是双实现或兼容路径，不需要为改名删除真实 target。

具体规则：

1. `Protocol` 不依赖其他 CabinFlow target；
2. `TransportApi` 只依赖 `Protocol`；
3. 具体 transport 只实现 `TransportApi`，彼此无依赖；
4. `Runtime` 依赖接口，不依赖 `TransportZmq`；
5. `Net` 不依赖协议或业务；`Gateway` 不依赖 AgentProtocol；
6. `apps/` 与测试装配具体 transport，库不在运行时探测或切换实现；
7. `agent/` 只消费公开 target 和公开头文件，不直接引用 `src/`；
8. 禁止新增 `common/`、`utils/`、`manager/` 杂物目录；已恢复的旧 utils 是迁移取证材料，不是允许新调用的共享库。

## 5. 公开 API 与内部实现

公开头文件必须位于：

```text
<module>/include/cabinflow/<module>/
```

只有跨 target 使用且具有稳定语义的类型才能成为公开 API。模块内部 helper 放在 `src/`，不为测试方便而公开内部实现。

包含方式保持唯一：

```cpp
#include <cabinflow/protocol/message_envelope.hpp>
#include <cabinflow/runtime/session_ledger.hpp>
#include <cabinflow/transport/transport.hpp>
```

禁止继续使用相对路径穿越模块目录，也不保留旧 include path 兼容别名。

## 6. 旧能力迁移映射与删除门槛

下表只说明职责归属；每项能力的当前状态和删除许可唯一记录在能力迁移账本，不能以整个目录已重写或不参与构建为删除理由。

| 历史路径 | 目标位置 | 处理方式 |
|---|---|---|
| `runtime/include/.../message_envelope.hpp` | `protocol/include/cabinflow/protocol/` | 更新 namespace/include 后直接迁移 |
| `runtime/include/.../session_ledger.hpp` | `core/include/cabinflow/runtime/` | 保留 Runtime 语义 |
| `runtime/src/session_ledger.cpp` | `core/src/` | 与头文件同模块迁移 |
| `runtime/include/.../version.hpp` | `core/include/cabinflow/runtime/` | 暂留，后续确认是否需要生成 |
| `hybrid-comm/` | `transport/zmq/`；控制行为归 `gateway/` | PUB/SUB 与旧 RPC 分别取证，不复制历史 API |
| `infra-controller/` | `protocol/`、`gateway/`、`core/` | 控制 schema、ControlService、Registry/work 生命周期分拆迁移；全局 KV 等候选裁剪项等待用户批准 |
| `unit-manager/` | `net/`、`gateway/`、未来 `apps/runtime_daemon/` | TCP、消息桥接与进程装配逐项迁移；远程 action/端口池不得顺带删除 |
| `network/` | `net/` | Reactor、TCP、连接器与线程池已进入新模块；旧行为取证输入按账本保留，未全部验收前不删除 |
| `node/` | Agent 节点迁到 `agent/` | Runtime 只保留通用 Node API |
| `node/test/`、`sample/` | `tests/contract/`、`tests/integration/` 对应行为 | 保留旧控制/网络样例用于 characterization（固定历史行为），不能用一个 echo demo 代替全部能力 |
| `utils/json.hpp`、`utils/sample_log.h` | 历史依赖取证材料 | 不进入活动构建，不新增消费者；按依赖和账本门槛处理 |
| 旧 `build.sh` | 依赖记录与 `scripts/` 的规范入口 | 不作为新构建入口；其历史依赖仍需逐项确认 |

迁移顺序：登记能力与保留范围 → 固定旧行为/新合同 → 实现新位置 → 切换全部活动调用方 → 运行对应测试并记证据 → 更新账本 → 检查删除许可。

只有能力达到 `verified`，或用户明确批准为 `approved-drop`，才能删除对应旧实现。删除目录前必须检查其中所有能力、样例和依赖；`pending`、`characterized`、`migrating` 均不构成删除许可。禁止暂存或提交未经许可的删除。

保留的 `infra-controller/`、`unit-manager/`、`network/`、`hybrid-comm/`、`node/test/`、`sample/` 和旧 utils 不接入活动根构建，也不由生产路径调用。历史取证材料不等于运行时兼容、双实现或回退；活动调用方仍须一次性切换，不保留旧接口别名。

## 7. 第一周结构建设记录与当前入口

下面仅保留最初结构建设顺序，已完成情况见第一周验收记录；它不是重新执行迁移/删除的指令。2026-10-02 用户已改为 Demo 优先，取代 2026-09-30 的先收尾/暂停模型安排。当前下一步见实施计划第 8 节，恢复真实模型工作；活动目录对齐仍不等于旧能力全部 verified。

### 第一步：建立 `protocol/`

- 迁移 `MessageEnvelope`；
- 冻结身份和消息类型；
- 保持现有 `SessionLedger` 测试可编译；
- target 名称为 `CabinFlow::Protocol`。

### 第二步：建立 `core/`

- 迁移 `SessionLedger`；
- 用真实 target `cabinflow_runtime` 定义库，并以 `CabinFlow::Runtime` alias 对外链接；
- 更新测试 include 和链接 target；
- 原嵌套目录按能力拆分；旧路径仅在第 6 节删除门槛满足后清理。

### 第三步：建立 Transport API 与进程内实现

- 只定义 demo 所需的最小接口；
- 明确 callback 线程和 subscription 生命周期；
- 用 `InMemoryTransport` 驱动 integration test。

### 第四步：加入 bounded queue 和取消

- 放入 `core/`；
- 使用 fake clock 消除依赖 sleep 的测试；
- 验证 session/work 取消范围和满队列策略。

### 第五步：建立 `runtime_demo`

- `text_source_node -> echo_processor_node`；
- app 负责选择 `InMemoryTransport`；
- 结构化日志串联 trace/session/work/message；
- demo 行为由 integration test 覆盖。

ZeroMQ 迁移排在上述链路通过之后。否则无法区分 Runtime 语义错误和 socket/线程错误。

## 8. 本结构刻意不包含的内容

- 不建立 plugin、factory registry 或动态加载系统；
- 不建立 Binder、SOME/IP 空目录；实际开发该适配器时再加入；
- 不提供旧 include path、旧协议和旧 transport 的兼容层；
- 不在 Runtime 中建立 ASR、LLM、TTS、vehicle 或 scene 目录；
- 不建立通用 `base/`、`shared/`、`misc/`；
- 不为未来可能使用的数据库、RPC 或配置中心预留抽象。

## 9. 活动结构检查（不是 Demo 的新增总门槛）

以下用于检查现有结构和维护模块边界，不要求先完成全部历史迁移才能接真实模型/界面。Demo 完成只按实施计划第 5 节验收；旧能力全部迁移另按账本验收，不能混为一谈：

1. 根构建装配上述 Runtime 模块及已说明的 Agent targets/apps/tests；历史模块不重新接入；
2. `agent/` 不引用 Runtime 的 `src/` 或历史目录；
3. 核心头文件中没有 ZeroMQ、Android 或业务模型依赖；
4. 所有活动调用方的 include 和链接关系已切换，无旧路径兼容；保留的历史取证输入不作为未切换调用方；
5. 原有 `SessionLedger` 行为测试继续通过；
6. transport/net 单测、Gateway/control/Target contract、TCP 与双节点集成测试覆盖对应活动能力；
7. `runtime/scripts/test.sh` 是标准 Debug 构建测试入口，ASan 用已有 `linux-asan` presets；窄测试与 TSan 命令见开发/验证文档，不新增平行构建脚本；
8. 记录实际 ASan/TSan 范围、环境及限制；测试通过不自动把账本状态改为 `verified`；
9. 每项旧能力都有所有者、目标位置、测试和状态；所有删除均有 `verified` 或 `approved-drop` 依据；
10. 一次性启停复用既有合同，非法业务 final 与 Demo 的取消/结果关联按当前实际路径解决；通用异步、重启、Pause 和长期资源保留不作为本轮总门槛，也不得作为既有保证。
