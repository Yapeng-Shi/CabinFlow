# ADR-006: Injected monotonic clock and reject-on-full queue

## Status

Accepted and implemented for the Runtime core.

## Decision

`SessionLedger` receives a `Clock` at construction and reads monotonic time
itself. Production composition uses `SteadyClock`; tests use `FakeClock` and
advance time without sleeping.

`BoundedQueue<T>` is a thread-safe, non-blocking FIFO. Its capacity is
mandatory and positive. `try_push()` returns `kFull` without replacing or
discarding an accepted item; it never waits for capacity. A caller can reserve
one slot before it mutates `SessionLedger`, then atomically commit or release
that slot. Therefore a queue-full rejection cannot leave a duplicate or
sequence record behind.

Runtime 同时更新 `SessionLedger` 的后续准入状态与 `CancellationRegistry` 的执行前
取消状态；队列本身不另建取消存储。worker 在调用 target 前检查取消，尚未开始
的排队消息被跳过，已开始执行的业务副作用不伪装为已回滚。

`Runtime::target_stats(name)` 提供只读的单 Target 快照：`queue_depth` 是已提交且
尚未开始处理的消息数，不含预留槽位或正在执行的消息；`queue_full_rejections`
统计因容量不足被拒绝的预留；`handling_count` 与
`handling_duration_ns_total` 只统计实际调用 `TargetNode::on_message()` 的次数
和总耗时。耗时使用注入的单调 `Clock`，测试可用 `FakeClock` 精确验证。
各字段并发读取安全，但快照不保证跨字段的同一时刻原子一致性；它不是性能报告
或外部监控后端。

## Consequences

- Expiry tests are deterministic and do not depend on scheduler timing.
- Backpressure is visible to the Gateway as a typed `QUEUE_FULL` rejection;
  it does not evict older work or retry automatically.
- Runtime owns one queue and one worker per `TargetNode`. Workers recheck TTL
  before calling the node, so an item that expired while waiting is rejected
  without invoking business code.
- 慢 Target 的契约测试固定队列深度、满队列拒绝和处理耗时；另有独立的
  `bounded_queue_stress_test` 检查多生产者/消费者守恒，不把测试内的重试写成生产策略。
