# 第一周 Runtime 验收记录

日期：2026-09-29。环境：WSL2 Ubuntu 24.04、x86、GCC 13.3.0；Debug 和
`linux-asan` 均在该环境构建。这里只验证本地 Runtime 路径，不代表真实座舱、
RK3576 或完整 Agent 流水线。

## 实际执行的命令与结果

以下命令从 `/home/projects/CabinFlow/runtime` 执行：

```bash
cmake --build --preset linux-debug -j2
ctest --preset linux-debug --output-on-failure
cmake --build --preset linux-asan -j2
ctest --preset linux-asan --output-on-failure
ctest --preset linux-debug --repeat until-fail:10 -I 15,18 --output-on-failure
ctest --preset linux-debug --repeat until-fail:10 -R runtime_demo_integration --output-on-failure
cmake --build --preset linux-debug --target bounded_queue_stress_test -j2
./build/linux-debug/tests/bounded_queue_stress_test
./build/linux-debug/apps/runtime_demo/runtime_demo
```

Debug CTest 26/26、ASan CTest 26/26 通过；所选 4 个并发/Gateway 测试
各重复 10 次，demo 集成测试重复 10 次，均通过。队列压力程序以容量 64、
4 个 producer、3 个 consumer、共 100,000 条消息检查数量与校验和守恒，退出码为 0；
这不是吞吐量或延迟基准。

另用现有手工配置的 TSan 构建重新编译并分别运行以下三个目标，均退出码 0，
未报告数据竞争；启动时使用 `setarch x86_64 -R`：

```bash
cmake --build build/linux-tsan --target target_runtime_contract_test session_ledger_test runtime_demo -j2
setarch x86_64 -R ./build/linux-tsan/tests/target_runtime_contract_test
setarch x86_64 -R ./build/linux-tsan/tests/session_ledger_test
setarch x86_64 -R ./build/linux-tsan/apps/runtime_demo/runtime_demo
```

## 八项退出验收

| 计划用例 | 当前证据 |
|---|---|
| 两个 session 并发，结果归属正确 | `runtime_demo_integration` 检查 driver/passenger 独立身份与回显。 |
| 重复 `message_id` 只接受一次 | demo 拒绝重复；`session_ledger_test` 还检查跨 session 全局去重。 |
| 同流乱序明确拒绝 | demo 的 `order-low` 返回 `stale_sequence`；Ledger 单测覆盖。 |
| deadline 到期后不开始新工作 | demo 返回 `expired`；`target_runtime_contract_test` 固定排队后过期不调用 handler。 |
| 取消一个 work 不影响同 session 其他 work | demo 的 `cancel-work` 被拒绝而 `other-work` 正常处理；取消契约测试覆盖排队场景。 |
| final 后 partial 被拒绝 | demo 的 `after-final` 返回 `stream_finalized`；Ledger 单测覆盖。 |
| 慢消费者触发预定背压 | `target_runtime_contract_test` 用阻塞 handler 固定 queue depth=1、一次 `QUEUE_FULL`、失败后可安全重试，避免随机 sleep。 |
| 测试可重复运行且不依赖随机 sleep | 相关并发/Gateway 用例及 demo 各重复 10 次通过；关键顺序以条件变量和注入时钟控制。 |

`runtime_demo` 仍只有 `text_source` 和 `echo_processor` 两个节点，使用
`InMemoryTransport`；真实 TCP Gateway、每 Target 的独立队列与 worker 由各自的
contract/integration test 验证，不把 demo 误称为真实模型或网络部署。

## 未验证边界与下一步

- 当前 26 项没有完成全量 TSan 验证；本轮三个定向目标及历史网络子集的
  范围见 `sanitizers.md`，不能据此推断整个 Runtime 无竞态。
- `Pause` 后数据准入、`target_node` 与 work `unit_id` 一致性尚未定案。
- 正式 `runtime_daemon`、旧能力迁移最终验收、真实 ASR/LLM/TTS、AAOS 和
  RK3576 不属于本周已验证结果。
- 按实施计划，下一主目标是第二周 x86 固定 WAV 到回答 WAV 的离线闭环；先用
  fake 节点固定消息与状态流，再逐个引入可复现的真实模型依赖。
