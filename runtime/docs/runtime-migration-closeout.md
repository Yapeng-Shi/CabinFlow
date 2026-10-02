# Runtime 迁移收尾记录

执行日期：2026-10-02；环境：WSL2 Ubuntu 24.04 / x86_64。
该次收尾任务的目标是 Runtime 迁移收尾，当时模型烟测与真实模型接入暂停。
随后用户于同日更新为 [Demo 优先计划](../../plans/IMPLEMENTATION_PLAN.md)，恢复模型定位；
本文保留该次历史验证范围，不再作为暂停模型工作的当前指令。
本记录不是整个迁移完成声明，也不是历史输入删除许可。

## 本轮完成

1. [旧依赖记录](legacy-dependencies.md)：固定旧 build.sh 指纹、声明与消费者，
   新增静态输入保留/根 CMake 排除检查。缺失 eventpp/simdjson 安装子脚本，
   安装能力仍 pending；未执行旧脚本、未安装依赖、未恢复旧生产构建。
2. 一次性实例：重复 start、失败/停止后重启、终态注册有明确错误；重复 stop
   不重复释放节点，stop 前未启动的实例也进入终态。
3. 生命周期锁边界：Starting/Stopping 冻结图/关闭准入，锁外启动、回滚、
   清队列和 join；外部并发 stop 等待唯一清理者发布终态。
4. 启动抛异常：已启动前缀逆序释放，失败节点自行清理局部资源，原异常传播。

## 问题、修正与验证

旧代码仅用 running 布尔值，失败后会再次启动节点。新单测先在旧行为下实际
失败（`failed runtime cannot restart`），状态修正后通过。六态将“未启动”和
“已停止/已失败”分开，不靠重置队列、取消历史或隐藏重试支持重启。

另一个锁环是 stop 持生命周期锁 join，而 worker/节点回调查询 Runtime 又要
取得同一锁。屏障固定 handler 正在执行及停止已经关闭准入，验证 running、
reserve_target、target_stats、failure/completion 回调与 Node::stop 查询均可返回；
stop 等 handler 完成后才释放节点。旧 reservation 的拒绝不写 Ledger，排队
消息不再调用 handler。

独立 Astra 预审与复审发现最初两处等待测试可能退化为串行。修正后测试等
`runtime.stop_waits` 实际选中等待分支，再释放启动/清理屏障，并检查返回时
资源已清理。计数仅表示分支选择，不表示已经睡眠或等待耗时。

Sol 在 `/tmp/cabinflow-lifecycle-mutations.icPgBD/` 构建两个临时故障变体；
保留计数递增，仅在 CV.wait 前分别对 Starting/Stopping 提前 return。
首次均退出 1，错误分别为 `stop returns only after startup or failure rollback`
及 `concurrent stop waits for full cleanup`；各重复 30 次均被拒绝，未改生产源。
这些有限调度测试不是任意线程交错的形式证明。

## 已执行命令与结果

Luna（实际模型 GPT-6 Luna）执行标准回归，Sol 负责测试设计/修正与整合；
Astra（实际模型 GPT-6 Astra）只读预审/复审，没有代替测试或编辑规范。

在 `/home/projects/CabinFlow/runtime` 执行：

```bash
./scripts/test.sh
cmake --preset linux-asan
cmake --build --preset linux-asan -j 4
ctest --preset linux-asan --output-on-failure
ctest --preset linux-debug -R '^runtime_lifecycle_contract_test$' \
  --repeat until-fail:30 --output-on-failure
```

最终修正后 Debug 与 ASan 全量均 29/29、退出码 0；生命周期重复 30 次通过。
新增的是 `legacy_dependency_inventory_test` 与 `runtime_lifecycle_contract_test`；
原 27 项继续通过。`git diff --check` 通过。

TSan 使用已有手工配置的 `build/linux-tsan`，并非新增 preset。确认缓存的
CXX/Debug flags 为 `-fsanitize=thread -fno-omit-frame-pointer`，链接 flag 为
`-fsanitize=thread`；不存在独立 TSan 选项不代表这些 flags 未启用。
首次构建因新测试目标尚未生成失败；在原缓存目录重新生成后构建成功：

```bash
cmake -S . -B build/linux-tsan
cmake --build build/linux-tsan --target runtime_test runtime_lifecycle_contract_test -j 4
setarch x86_64 -R ./build/linux-tsan/tests/runtime_test
setarch x86_64 -R ./build/linux-tsan/tests/runtime_lifecycle_contract_test
```

两个用例各执行一次，退出码均为 0，没有 TSan 报告。仅此 2 项，不是全量
29 项 TSan 通过；没有改缓存中的 sanitizer flags 或安装依赖。

## 未完成边界

- worker/交付回调/节点生命周期回调直接 stop：未批准，D5 未全部关闭。
- 线程创建失败后的部分 worker 回滚：未动态注入验证。
- 启动异常与 stop 等待交错覆盖首节点抛异常；成功前缀的逆序回滚另有测试，
  尚未固定非空回滚正在进行时另一个 stop 的等待交错。
- Pause、非法业务 final、异步输出、资源历史保留：仍按实施计划 D6～D9 冻结。
- 已批准的外部首条所属 Unit 校验、旧控制/TCP/ZMQ 逐能力覆盖、正式 daemon
  以及候选裁剪项：仍待后续收尾，不以目录对齐或本次回归自动标为 verified。
- 未测试 RK3576、AAOS 或真实离线语音闭环；本轮不提交/推送、不删除旧输入。
