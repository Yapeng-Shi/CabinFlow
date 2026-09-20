# CabinFlow 实施计划

> 面向智能座舱的全离线多乘员语音 Agent 与 C++ 服务框架

## 1. 项目目标

CabinFlow 不是普通车机 UI 项目，也不以复刻任何车企私有系统为目标。项目用于构建一个公开、可复现、可测试的智能座舱原型，重点展示两类能力：

1. C++ 座舱 AI Runtime：消息协议、会话隔离、取消、背压、生命周期、故障处理与可观测性；
2. 离线座舱 Agent：ASR、RAG/LLM、TTS、车辆状态访问、安全 Tool Call 和场景编排。

目标岗位是智能座舱 AI/语音基础设施、C++ 中间件和端侧推理相关岗位。项目开发同时服务于工程交付和知识学习：每个里程碑必须能运行、能测试、能解释、能形成证据。

## 2. 事实边界

### 当前已实现并验证

- 原 `Edge-LLM-Infra` 与 `Edge_LLM_RAG_Voice` 已迁移为 `runtime/` 和 `agent/`；
- Runtime 已有 CMake Preset、构建脚本和 CTest 入口；
- 已有 transport-neutral `MessageEnvelope`；
- 已有内存态 `SessionLedger`，覆盖身份校验、TTL、顺序、去重、final 后关闭和 session 级取消；
- 2026-09-19 在 WSL2 Ubuntu 24.04 执行 `runtime/scripts/test.sh`，2/2 测试通过。

### 当前存在但未形成完整闭环

- Agent 中存在 ASR、RAG、LLM、TTS 和 ZeroMQ 历史模块；
- 部分模型文件缺失，旧模块依赖尚未完全纳入可复现构建；
- Agent 尚未形成统一协议下的一键式端到端流水线。

### 目标能力，不作为当前成果

- AAOS Emulator、AIDL/Binder、VHAL；
- SocketCAN/vcan 和 SOME/IP Adapter；
- 多乘员区域输入、场景编排、流式打断和故障恢复；
- RK3576、RKNN、RKLLM 实机部署及性能数据。

在没有 RK3576 的情况下，只做接口设计、交叉编译准备或目标后端适配；不得写成实机部署结果。AAOS Emulator、fake vehicle、vcan 和 mock 也必须标记为模拟验证。

## 3. 目标架构与职责边界

```text
AAOS HMI / CLI / Test Driver
             |
             v
      Cockpit Agent Service
  session / dialogue / policy
             |
             v
       CabinFlow Runtime
 envelope / lifecycle / cancel
 transport / queue / tracing
             |
       +-----+-------------------+
       |                         |
       v                         v
ASR -> Router -> RAG/LLM -> TTS  Tool/Scene Orchestrator
                                 |
                                 v
                     FakeVehicle / vcan / VHAL
```

`runtime/` 只拥有通用基础设施，不包含真实 ASR、LLM 或车控业务。`agent/` 拥有 AI 流水线、对话状态、安全策略和车辆能力编排，不重复实现 Runtime 的消息、会话和生命周期机制。

统一身份语义：

- `trace_id`：一次端到端链路，可跨节点关联日志；
- `session_id`：一个乘员/区域的连续会话边界；
- `work_id`：会话内一次用户请求或一次可取消工作；
- `message_id`：单条消息的唯一身份，用于去重和诊断；
- `sequence`：同一有序流中的单调序号，不替代唯一身份。

## 4. 开发原则

- 先打通 x86 Linux/WSL 的确定性闭环，再进入 Android 和目标硬件适配；
- 每阶段只有一条 canonical data path，不保留旧实现回退；
- 对必需配置和依赖显式失败，不用静默默认值掩盖错误；
- 先以 fake node 和固定 WAV 建立可重复测试，再接真实模型；
- LLM 只生成结构化意图，不直接接触车辆执行接口；
- 车控必须经过 Schema、白名单、区域权限、车辆状态和幂等校验；
- 每项指标记录环境、版本、样本、并发、计时边界和统计口径；
- 不为堆砌简历关键词引入框架或协议。

## 5. 六周里程碑

| 周次 | 主题 | 可演示结果 | 完成判据 |
|---|---|---|---|
| 1 | Runtime 通信底座 | 多会话消息可传递、取消、超时和追踪 | 协议冻结；核心状态测试通过；最小双节点 demo 可运行 |
| 2 | x86 离线语音闭环 | 固定 WAV 到回答 WAV | 一条命令运行；各阶段有计时；失败显式可定位 |
| 3 | 车辆能力与安全网关 | 查询和修改模拟车辆属性 | 10 个属性；非法调用拦截；重复请求不重复执行 |
| 4 | 多乘员与场景 Agent | 多区域、多指令、小憩模式、打断 | 会话不串线；部分失败有明确结果；取消传播可验证 |
| 5 | AAOS Emulator | HMI 展示状态并调用 Native 服务 | AIDL/Binder 通路、车辆属性展示、进程断连可观测 |
| 6 | 可靠性、性能与求职材料 | 故障演示、指标报告、视频和 README | ASan/TSan；P50/P95；边界清晰的简历描述 |

## 6. 第一周：Runtime 通信底座

### 本周目标

把现有 `MessageEnvelope + SessionLedger` 扩展为可支撑 Agent 节点通信的最小 Runtime。第一周不接真实模型、不实现 AAOS、不引入 Binder/SOME-IP，也不设计通用插件系统。

本周主数据流：

```text
Producer -> MessageEnvelope -> ITransport -> Consumer
                |                   |
                v                   v
          SessionLedger      bounded queue
                |
       deadline / cancel / final
```

### Day 1：冻结协议与状态语义

任务：

- 复核现有 ADR、`MessageEnvelope` 与 `SessionLedger`；
- 明确 request、partial、final、error、cancel 五类消息；
- 定义 `trace_id/session_id/work_id/message_id/sequence` 的约束；
- 明确 deadline 使用绝对时间还是剩余时长，并统一时钟语义；
- 补齐非法身份、过期消息、final 后消息、重复消息和乱序消息测试表。

交付：

- 一份更新后的消息协议 ADR；
- 一张状态转移表；
- 参数化单元测试清单。

验收：

- 任意字段都能回答“由谁生成、作用域是什么、何时失效”；
- 不存在两个字段表达同一个概念；
- 协议错误返回显式错误，不静默修正输入。

学习重点：消息身份、幂等与顺序是三个不同问题；先定义不变量，再写传输代码。

### Day 2：最小 Transport 接口与内存实现

任务：

- 定义满足当前需求的最小 `ITransport`；
- 支持 publish/subscribe 或 request/reply 中本周 demo 实际需要的语义；
- 编写确定性的 `InMemoryTransport`，仅用于测试和进程内 demo；
- 明确 callback 执行线程、订阅对象生命周期和关闭顺序。

交付：

- Transport 接口；
- `InMemoryTransport`；
- 订阅、投递、取消订阅和关闭测试。

验收：

- 业务节点不包含 ZeroMQ 头文件；
- callback 不在持锁状态下执行；
- transport 关闭后不会调用已销毁对象。

学习重点：接口的价值是隔离业务语义和传输语义，不是提前支持所有中间件。

### Day 3：有界队列与背压

任务：

- 为节点输入建立有界队列；
- 明确满队列策略，只实现选定的一种策略；
- 增加队列深度、拒绝次数和处理耗时观测值；
- 用慢消费者测试生产速度超过消费速度的行为。

交付：

- 有界队列或最小 mailbox；
- 慢消费者压力测试；
- 背压决策 ADR。

验收：

- 内存不会随消息数量无限增长；
- 满队列时调用方收到明确结果；
- 测试能稳定复现并验证背压，而不是依赖人工观察。

学习重点：背压是容量边界和责任归属问题，不等同于“加一个队列”。

### Day 4：deadline 与取消传播

任务：

- 将 deadline 检查放在接收、入队和执行前的明确边界；
- 以 `work_id` 为最小取消单位，支持按 `session_id` 批量取消；
- 定义取消前、执行中和 final 后取消的返回语义；
- 添加竞态测试：cancel 与 partial/final 同时发生。

交付：

- 取消令牌或状态接口；
- deadline/cancel 测试矩阵；
- 一条可观察的取消链路。

验收：

- 取消不会跨 session 影响其他工作；
- final 之后不能重新打开流；
- 已执行的副作用不会被伪装成“撤销成功”。

学习重点：取消是协作协议，不是强制终止线程；有副作用的操作需要补偿而非假回滚。

### Day 5：双节点 demo 与周验收

任务：

- 实现 `text_source -> echo_processor` 最小双节点 demo；
- 输出结构化日志，至少包含时间、节点、trace、session、work、message 和状态；
- 加入双 session 并发、重复消息、乱序、超时和取消场景；
- 运行 CTest，并在 Debug 构建下执行 ASan；TSan 若受环境限制则记录为未验证。

交付：

- 一条命令构建和测试；
- 一条命令运行 demo；
- 第一周验收记录，包含命令、环境、结果和未完成边界。

验收用例：

1. 两个 session 并发发送消息，结果归属正确；
2. 重复 `message_id` 只接受一次；
3. 同一流乱序消息被明确拒绝；
4. deadline 到期后不再开始新工作；
5. 取消一个 `work_id` 不影响同 session 的其他工作；
6. final 后的 partial 被拒绝；
7. 慢消费者触发预定背压策略；
8. 所有测试可重复运行且无随机 sleep 依赖。

本周退出条件：上述 8 项通过，ADR 与实现一致，构建入口保持单一。否则不进入真实 ASR/LLM 集成。

## 7. 第二周：x86 离线语音闭环

目标数据流：

```text
WAV -> ASR -> Router -> RAG or Local LLM -> sentence chunk -> TTS -> WAV
```

实施顺序：

1. 用 fake ASR/LLM/TTS 接入 Runtime，先验证消息和状态流；
2. 为模型依赖提供可复现的获取方式和校验值；
3. 逐个替换为真实 ASR、RAG/LLM、TTS；
4. 每替换一个节点都保留相同的契约测试，不保留旧生产路径；
5. 增加阶段计时：ASR、检索、首 token、首句、TTS、端到端。

验收：固定 WAV 可重复生成回答 WAV；缺模型或配置时显式失败；一次运行可通过 `trace_id` 串联全链路日志。

## 8. 第三周：车辆能力与安全网关

实现 `FakeVehicleService` 和 10 个有明确类型、范围、读写权限的车辆属性。候选属性：车速、挡位、四区温度、座椅加热、车窗、媒体音量和胎压。

安全调用链：

```text
LLM JSON -> schema validation -> tool allowlist -> zone permission
         -> vehicle-state precondition -> idempotency -> execution -> receipt
```

验收：

- 非法工具名、非法参数、越权区域和行驶中危险操作全部被拦截；
- 相同幂等键重复提交不会产生重复副作用；
- 执行结果区分成功、拒绝、超时和部分失败；
- vcan 只在 FakeVehicle 稳定后加入，不取代确定性测试。

## 9. 第四周：多乘员、复合指令与场景编排

优先完成四个可演示场景：

1. 多乘员区域隔离：由测试驱动或模拟器注入 `speaker_zone`，不声称声源定位；
2. 一句话多指令：生成多个 Tool Call，表达依赖关系并汇总每项结果；
3. 小憩模式：验证前置条件，按步骤执行，失败时执行显式补偿；
4. 语音打断：取消未完成生成、合成和播放，对已执行车控发起补偿操作。

验收：四并发 session 无串线；复合指令的部分失败可定位；取消延迟有明确起止点；补偿失败会被报告而非隐藏。

## 10. 第五周：AAOS Emulator

第一版只实现最小 Android 集成：

- 一个 HMI，展示转写文本、Agent 状态、车辆状态、调用历史和延迟；
- 一个 AIDL 接口连接 Android 与 Native C++ Agent Service；
- 模拟车辆属性或 CarPropertyManager 订阅；
- Binder 断连可被检测并显示。

时间充足后再考虑 VHAL 扩展属性、Audio Focus、多屏和 DeathRecipient。无需编译完整 AOSP，也不把 Emulator 结果写成量产车机验证。

## 11. 第六周：可靠性、评测与求职交付

可靠性测试：

- 节点崩溃或主动退出；
- 重复、乱序、延迟和丢失消息；
- 队列满载和慢消费者；
- 多 session 并发取消；
- Binder 断连；
- ASan/TSan 可执行范围内的内存与竞态检查。

评测指标只选择已经实现并能稳定采样的项目：

- 多指令结构化解析成功率；
- 区域路由准确率；
- 非法车控拦截率；
- 重复 Tool Call 实际执行次数；
- 场景成功率和补偿结果；
- 打断停止延迟；
- 四 session 串线数；
- 节点恢复时间；
- VAD End 到首段播放的 P50/P95；
- CPU 与内存峰值。

每项性能结果必须记录：设备、OS、编译类型、模型及版本、输入集、样本数、并发度、计时边界和统计方法。

最终交付：

- 可复现 README；
- 架构图与关键时序图；
- 自动化测试和评测脚本；
- 故障注入记录；
- 3 至 5 分钟演示视频；
- 一页项目复盘：困难、根因、解决、测试/结果、边界；
- 20 秒项目介绍、90 秒展开和高频追问。

## 12. 每周固定工作方式

每周一：选择一个主目标，冻结接口和完成判据。

每个改动：

1. 写出要保护的不变量；
2. 先构造可重复失败或契约测试；
3. 实现最小修改；
4. 运行窄测试和受影响构建；
5. 检查旧符号、死分支和意外兼容逻辑；
6. 记录结果与未验证边界。

每周五生成一份周报，固定结构：

```text
目标：
完成：
未完成及原因：
关键困难：
根因：
解决：
测试和结果：
仍未验证的边界：
本周学到的机制：
调试方法：
工程权衡：
下周唯一主目标：
```

## 13. 简历证据门槛

只有达到以下门槛才写入完成时态：

- 功能：有源码、可执行入口和至少一个自动化测试；
- 性能：有可重复脚本、完整环境和统计口径；
- 可靠性：能够主动注入故障并观察预期行为；
- Android：在指定 AAOS Emulator 镜像上完成验证；
- 硬件：在真实设备上记录型号、系统、运行库和模型版本。

推荐最终口径应基于届时真实结果调整。当前只能描述为：

> 正在构建面向智能座舱的离线多乘员语音 Agent，将 C++ 消息运行时与 ASR、RAG/LLM、TTS 及模拟车辆服务整合，并通过 x86 Linux/WSL 的自动化测试逐步验证会话隔离、取消和安全 Tool Call。

不得使用“复刻 NOMI”“基于 SkyOS”“已部署 RK3576”或未经测量的性能数字。适合的边界表达是：

> 参考公开的量产智能座舱多乘员交互和跨域服务编排需求进行设计。

## 14. 立即执行项

下一次开发从第一周 Day 1 开始：复核已有 ADR 和测试，补齐协议状态表与测试矩阵。开始编码前先确认现有 `MessageEnvelope` 和 `SessionLedger` 是否已经满足本文身份语义；不满足时直接迁移并同步更新所有调用方，不增加兼容层。
