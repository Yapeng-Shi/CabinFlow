# CabinFlow Runtime 目录结构设计

## 1. 设计目标

Runtime 负责座舱 Agent 的通用运行基础设施：消息契约、传输边界、节点生命周期、会话与任务隔离、取消、背压和可观测性。它不包含 ASR、RAG、LLM、TTS、对话策略或车辆业务。

目录按稳定职责拆分，每个主要目录对应一个独立 CMake target。这样能够在编译期限制依赖方向，也能让单元测试只链接被测模块。

## 2. 目标结构

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
│   │   └── runtime_envelope.proto
│   ├── include/cabinflow/protocol/
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
│   │   └── version.hpp
│   └── src/
│       ├── cancellation.cpp
│       ├── node.cpp
│       ├── runtime.cpp
│       └── session_ledger.cpp
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
│       ├── text_source_node.cpp
│       └── echo_processor_node.cpp
│
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── protocol/
│   │   ├── transport/
│   │   ├── core/
│   │   └── observability/
│   ├── integration/
│   │   ├── two_node_flow_test.cpp
│   │   └── cancellation_flow_test.cpp
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
│   └── adr/
│
└── scripts/
    ├── build.sh
    └── test.sh
```

目录只在对应实现开始时创建，不预先生成大量空目录和占位文件。

## 3. 各层职责

### `protocol/`

拥有跨节点传递的数据契约和编解码：

- `MessageEnvelope` 及消息类型；
- `trace_id/session_id/work_id/message_id/sequence`；
- deadline、final、error、cancel 等协议字段；
- Protobuf schema 与 C++ domain type 的转换；
- schema version 校验。

不负责消息是否应该被业务处理，也不维护 session 状态。

### `transport/`

拥有消息“如何送达”的抽象和具体实现：

- `ITransport` 接口；
- subscription 生命周期；
- transport 层错误；
- 进程内确定性实现；
- ZeroMQ 实现及其 I/O 线程所有权。

业务代码只能依赖 `cabinflow_transport_api`，不能包含 ZeroMQ 头文件。`in_memory` 用于测试和本地 demo，不是 ZeroMQ 失败后的运行时回退。

### `core/`

拥有消息进入节点后的运行语义：

- `SessionLedger` admission decision；
- bounded queue 和背压；
- work/session 取消；
- node lifecycle；
- callback 调度和对象生命周期；
- deadline 检查；
- Runtime 启停和资源释放顺序。

`core/` 不知道 Protobuf socket、ZeroMQ endpoint 或 Agent 的业务 payload。

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

`main.cpp` 不实现队列、协议或业务算法。第一周只有一个 `runtime_demo`，不创建多个功能重叠的示例程序。

### `tests/`

- `unit/`：单模块、不启动真实外部服务；
- `integration/`：多个 Runtime 组件之间的数据流；
- `stress/`：并发、容量和竞态边界，不作为普通单元测试运行时间的负担；
- `support/`：仅测试使用的 fake 和 builder，生产 target 不得链接。

## 4. CMake target 与依赖方向

建议 target：

```text
CabinFlow::Protocol
CabinFlow::TransportApi
CabinFlow::TransportInMemory
CabinFlow::TransportZmq
CabinFlow::Observability
CabinFlow::Runtime
```

允许的依赖：

```text
Protocol
   ^
   |
TransportApi       Observability
   ^                    ^
   |                    |
TransportInMemory       |
TransportZmq            |
          ^             |
           \           /
             Runtime
                ^
                |
          apps / integration tests
```

具体规则：

1. `Protocol` 不依赖其他 CabinFlow target；
2. `TransportApi` 只依赖 `Protocol`；
3. 具体 transport 只实现 `TransportApi`，彼此无依赖；
4. `Runtime` 依赖接口，不依赖 `TransportZmq`；
5. 只有 `apps/` 决定使用哪一个具体 transport；
6. `agent/` 只消费公开 target 和公开头文件，不直接引用 `src/`；
7. 禁止创建 `common/`、`utils/`、`manager/` 作为无法说明所有权的杂物目录。

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

## 6. 当前目录迁移映射

| 当前路径 | 目标位置 | 处理方式 |
|---|---|---|
| `runtime/include/.../message_envelope.hpp` | `protocol/include/cabinflow/protocol/` | 更新 namespace/include 后直接迁移 |
| `runtime/include/.../session_ledger.hpp` | `core/include/cabinflow/runtime/` | 保留 Runtime 语义 |
| `runtime/src/session_ledger.cpp` | `core/src/` | 与头文件同模块迁移 |
| `runtime/include/.../version.hpp` | `core/include/cabinflow/runtime/` | 暂留，后续确认是否需要生成 |
| `hybrid-comm/` | `transport/zmq/` | 只提取仍需要的实现，不复制历史 API |
| `infra-controller/` | `core/`、`apps/` 或删除 | 按职责拆解，禁止整体包一层适配器 |
| `unit-manager/` | `apps/` 或删除 | 启动装配留下，通用逻辑进入 core |
| `network/` | 对应 transport 或删除 | 不保留第二套网络抽象 |
| `node/` | Agent 节点迁到 `agent/` | Runtime 只保留通用 Node API |
| `sample/` | `apps/runtime_demo/` | 只保留一个受测试的最小 demo |
| `utils/json.hpp` | 具体消费者或删除 | 不建立全局 utils 目录 |

每次只迁移一个有测试保护的能力：更新所有仓库内调用方、删除旧路径、运行测试。不得同时保留新旧接口或双实现作为回退。

## 7. 第一周实际落地顺序

### 第一步：建立 `protocol/`

- 迁移 `MessageEnvelope`；
- 冻结身份和消息类型；
- 保持现有 `SessionLedger` 测试可编译；
- target 名称为 `CabinFlow::Protocol`。

### 第二步：建立 `core/`

- 迁移 `SessionLedger`；
- 将当前 `cabinflow_runtime` 改为 `CabinFlow::Runtime`；
- 更新测试 include 和链接 target；
- 删除原来的嵌套 `runtime/` 子目录。

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

## 9. 完成判据

目录重构完成必须同时满足：

1. 根构建只包含明确列出的新 target；
2. `agent/` 不引用 Runtime 的 `src/` 或历史目录；
3. 核心头文件中没有 ZeroMQ、Android 或业务模型依赖；
4. 所有仓库内 include 和链接关系已迁移，无旧路径兼容；
5. 原有 `SessionLedger` 行为测试继续通过；
6. 新增 transport、背压、取消和双节点集成测试；
7. `runtime/scripts/test.sh` 是唯一标准验证入口；
8. ASan 验证通过，TSan 结果按实际环境单独记录。
