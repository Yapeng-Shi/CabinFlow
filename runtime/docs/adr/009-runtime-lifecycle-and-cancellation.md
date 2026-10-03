# ADR-009: Runtime owns node lifecycle and dual-scope cancellation

## Status

Accepted and verified on the WSL x86 development environment.

2026-09-30 补充决策：用户批准 Runtime 为一次性实例。运行中重复 `start()`
明确报错；启动失败或 `stop()` 后禁止再次 `start()`；重复 `stop()` 幂等。
此补充决策的外部同步启停已于 2026-10-02 实现并通过相应 Debug/ASan 测试，
验证范围见 [收尾记录](../runtime-migration-closeout.md)，不由之前的通过结果推导。
worker 自身请求停止的方式尚未确认；一次性实例决策不自动批准该路径。

### 一次性实例与外部同步启停

- 内部状态为 Ready / Starting / Running / Stopping / Stopped / Failed；只有
  Running 接收数据，只有 Ready 接收节点注册。状态不是新的公开 API。
- Starting/Running 重复 start 返回 `kAlreadyStarted`；Stopping/Stopped/Failed
  返回 `kLifecycleEnded`。未启动前 stop 也消耗实例，不重建队列或重置 Ledger。
- Starting 冻结图，节点启动和回滚在锁外执行；失败回滚完成后才发布 Failed。
  Node::start 抛异常时只清理已成功启动的前缀，然后原样传播，不吞错或重试。
- Stopping 在锁内关闭准入，由一个外部调用者在锁外清队列、join worker、
  逆序 stop 节点。其他外部 stop 等待启动/清理完成；返回时清理已完成。
  同步 stop 不强制中断已经执行的 handler。
- `runtime.stop_waits` 记录 stop 选择等待路径的次数，不表示等待时长、
  内核睡眠次数或清理完成次数；并发测试用它确认等待分支已被选中。
- 本合同只验收外部同步生命周期；worker、交付回调和节点生命周期回调直接
  调用 stop 的处理仍待确认，不新增异步停止接口、不把 self-join 当作支持。

## Decision

`CabinFlow::Runtime` owns a set of generic `Node` instances and gives each
node a `NodeContext` rather than letting applications construct per-node
transport, ledger, and logger dependencies independently.

- Nodes are registered before `start()`. Runtime starts them in registration
  order and stops them in reverse order.
- If any node fails to start, Runtime stops only the nodes that had previously
  started and returns `kNodeStartFailure`; it does not leave a partial graph
  running or attempt a second implementation as fallback.
- Node registration after Runtime has started is rejected. This keeps the
  dependency graph and its shutdown ordering explicit for the first version.
- Runtime owns both `SessionLedger` and `CancellationRegistry`.
  `cancel_session` and `cancel_work` update both in one API call.
- Runtime also owns the first `InMemoryMetrics` sink. Lifecycle, cancellation,
  and demo message results increment concrete counters without introducing an
  external monitoring dependency.
- `SessionLedger` rejects future delivery at the admission boundary.
  `CancellationToken`, scoped by `(session_id, work_id)`, lets an in-flight
  node observe the same monotonic cancellation state at its own safe point.

## Consequences and boundaries

- Cancellation has no reset operation. A new conversational turn must receive
  a new `work_id`; reusing an old work ID after cancellation is invalid.
- Runtime 不把网络 `MessageKind::kCancel` 自动解释为取消命令。当前唯一入口是
  Gateway 校验并执行类型化 `Exit`；`ControlService` 成功转换 Registry 状态后，
  Gateway 在发送成功响应前显式调用 `Runtime::cancel_work()`。后续输入由 Ledger
  拒绝，排队但未开始的消息由 worker 跳过；正在执行的 handler 仍需协作停止。
- 当前单轮 Agent 先通过不透明 cancel hook 在应用锁上与候选结果提交线性化，再由
  ControlService 处理有效 Exit；它不把 ExitResponse 当作 handler 已退出。
  根 completion 等所有下游返回后清理并发布唯一业务终态。
- 等待下游 Runtime 队列的同步根任务必须先由应用关闭准入、取消并 drain，再调用
  `Runtime::stop()`。否则 stop 丢弃下游队列而根仍在等 completion，会造成 join 死锁。
  这不是新的 Runtime stop API；server sigwait 外部线程和 Pipeline 测试覆盖当前装配。
- A node that starts asynchronous work must make `stop()` wait for, cancel, or
  otherwise make its own callbacks safe before returning. Runtime provides
  ordering, not a generic solution for arbitrary business-thread ownership.

| 取消相对消息的时序 | 当前结果 |
|---|---|
| work 取消先于输入准入 | Ledger 返回 `WORK_CANCELLED`，不入队。 |
| 消息已排队但 handler 尚未开始 | worker 调用前返回 `WORK_CANCELLED`，不执行 handler。 |
| handler 已开始后取消 | token 可被业务节点协作检查；Runtime 不强制终止或回滚已执行副作用。 |
| final 已准入后取消 | 取消状态保持；后续输入优先返回 `WORK_CANCELLED`，不会重新打开 final 流。 |

`session_ledger_test` 用无随机 sleep 的双线程起跑门验证 cancel 与 partial/final
并发时两种线性化结果，以及取消完成后的 work/session 隔离；
`target_runtime_contract_test` 固定排队消息与执行中 handler 的边界。
- `runtime_test` verifies failure rollback, reverse shutdown, the immutable
  post-start graph, and one cancellation reaching both token and ledger.
  `cancellation_test` verifies session/work isolation and idempotency;
  `metrics_test` verifies the in-memory counter behavior.
