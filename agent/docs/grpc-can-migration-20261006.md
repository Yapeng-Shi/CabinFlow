# gRPC / CAN 迁移记录（2026-10-06）

> 2026-10-07 后续验收：用户批准并完成同基线 WSL 内核/模块切换，实际 vcan ECU、
> gRPC/本地模型/Wayland Qt 回执闭环与限定 CAN ASan/TSan 已通过。
> 首次实总线暴露的 CAN data filter 错误已修复；[环境、故障回归与证据](wsl-vcan-20261007.md)。
> 下文 2026-10-06 的“无 vcan/待验收”描述是当日历史状态，不再是当前环境门槛。

## 当前边界

本轮经批准替换外部 Qt/Gateway 通信，不改 Runtime Core、Ledger、队列或
内部 InMemory/ZeroMQ。唯一产品入口为 `RuntimeGateway.Connect` 双向 gRPC 流，
消息仍是 `RuntimeMessage`；旧 TCP framer 仅参与 characterization 测试，
不链接生产 Gateway，不提供回退。Gateway 不依赖 AgentProtocol。

```text
Qt GUI ← queued callbacks ← gRPC CQ thread
  ↕ RuntimeMessage bidi stream（仅 loopback）
Gateway → control service / queue reservation → Ledger → Runtime worker
  → VoicePipeline → VehicleBackend → SocketCAN vcan0 → 独立 vehicle_ecu
  ← 类型化任务终态 / 可选车辆事实 ← ECU ACK
```

每流一读一写；消息上限 4 MiB；输出队列上限 8 条，包含正在写出的对象。
响应 sequence 分配与入队为同一临界区。满队列以 `RESOURCE_EXHAUSTED` 终止流；
不重连、不重试、不重放业务动作。Qt 网络等待在专属 CQ 线程，GUI 不等待 RPC。
断连只解除输出目的地，不隐式取消所有 work。

生产应用仅装配 SocketCanVehicle，FakeVehicle 属于测试库。启动必须收到 ECU 查询回执。
标准 11 位 Classical CAN：请求 0x600、响应 0x601，DLC=8；版本、操作/状态、
BE 事务号、值/状态位与保留字节详见 [Vehicle 合同](../vehicle/README.md)。
仅允许 kernel link kind 为 `vcan` 的 vcan0，不能通过重命名物理 CAN 接口绕过边界。

请求只写一次，ACK 等待 2 秒。写入前取消不执行；写入后不回滚，等待回执或超时。
取消检查与 nonblocking write 在同一提交 gate 中，ACK 等待不持有 Pipeline 锁。
超时后的状态与是否执行均未知：Protobuf optional bool 缺席，不能显示为关/未执行。
后续车辆变更被拒绝，用户通过“刷新车辆状态”查询后恢复；普通问答不伪造车控事实。
刷新使用正常 Gateway/Runtime 准入，但跳过 ASR/LLM/TTS。

## 实际环境与依赖

WSL Ubuntu 24.04 x86_64；GCC 13.3.0；CMake/CTest 3.28.3；
Ubuntu gRPC 1.51.1；Protobuf 3.21.12。
本轮安装 `libgrpc++-dev`、`protobuf-compiler-grpc`、`can-utils` 及其依赖。

```bash
sudo apt-get install libgrpc++-dev protobuf-compiler-grpc can-utils
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR="$PWD/runtime/build/inference" \
  -DCMAKE_IGNORE_PREFIX_PATH=/mnt/d/ProgramData/Anaconda3/Library \
  -Dre2_DIR=re2_DIR-NOTFOUND
cmake --build runtime/build/voice-demo -j4
ctest --test-dir runtime/build/voice-demo --output-on-failure
```

上述 RE2 参数排除本机 Windows Anaconda 的 CMake 配置污染，不是另一套运行时实现。
新克隆还需按既有指南准备模型/SDK/Qt；这里不自动下载或替换模型。

## 2026-10-06 的 WSL vcan 前提（次日已解除）

迁移当日内核 `6.6.87.2-microsoft-standard-WSL2` 明确 `CONFIG_CAN_VCAN is not set`，
系统没有 vcan0。因此真实 ECU / SocketCAN 集成和迁移后的全模型 GUI 闭环未验收。
确定性 codec/backend 单测与真实 gRPC 测试不代表总线或硬件验证。

当时需要另行批准 WSL 自定义内核、全局 `.wslconfig` 修改及 WSL 重启。
2026-10-07 用户已批准并完成该环境变更与实际总线验收。
本轮没有做这些操作，也没有换 VM 或添加 FakeVehicle 生产回退。
准备内核时按 [Microsoft 内核构建说明](https://github.com/microsoft/WSL2-Linux-Kernel)
选择与本机 WSL 匹配的版本、构建 CAN_RAW/VCAN 与匹配模块；
[WSL 全局配置说明](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)
说明自定义 kernel/kernelModules 设置。不要只替换内核却沿用不匹配模块。

以下命令在 2026-10-06 尚未执行；2026-10-07 已在新内核实际执行，重启后仍须重建临时接口：

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set dev vcan0 up
ip -details link show dev vcan0
bash agent/scripts/voice_demo.sh start
bash agent/scripts/voice_demo.sh stop
```

启动脚本只验证既有 vcan0，按 ECU→server→UI 启动，按 UI→server→ECU 停止。
仅清理经过 PID/可执行文件/uid 核验的自有进程；不删除接口或控制其他播放器。
Qt SIGINT/SIGTERM 进入 GUI 关闭流程，等待语音、音乐及 gRPC 收敛。

## 验证记录

- 独立 Gateway 编译通过；完整 Demo 的 server/UI/CLI/ECU 编译通过。
- Vehicle 单测通过；无 vcan 时集成测试与 ECU 启动明确失败，不当作成功跳过。
- 最终完整 Debug：37/37，通过。Qt 断连断言曾早于 GUI 收到 CQ 通知，
  修正为等待连接与 GUI 清理事实共同收敛，不削弱状态断言。
- `grpc_stream_contract_test` 独立 Debug 连续 30 轮通过，每轮 20 次双 worker/断连竞争。
  受控 gRPC 1.51.1 流控 fixture 禁用预读与 BDP，验证 3 MiB 首写在途时前 8 条准入、
  第 9 条拒绝并返回 RESOURCE_EXHAUSTED；并验证同 topic 序号、OnDone 迟到输出与所有权释放。
- 限定 ASan：控制/数据/语音 gRPC、Pipeline、CAN、音乐测试通过；新 stream 合同通过。
  Qt 测试的业务断言已通过，但 LeakSanitizer 首轮报 PulseAudio 176 字节 / 2 次分配，
  schema 修复后的最终复测报 264 字节 / 3 次分配，
  栈经 QAudioOutput 初始化，未抑制、未标记 ASan 全绿。原始记录在
  `runtime/build/grpc-can-asan/ctest-voice-client.log` 与最终
  `runtime/build/grpc-can-asan/Testing/Temporary/LastTest.log`。
- TSan 默认启动报 unexpected memory mapping；使用 `setarch x86_64 -R` 后 CAN 单测通过，
  三项 gRPC 测试报告大量警告（首次栈含系统 gRPC epoll/eventfd）。不能仅因首条属于依赖就
  宣称全部是第三方误报，更不能宣称无竞态；完整日志
  `runtime/build/grpc-can-tsan/ctest-setarch.log` 留待独立审查。
- `bash -n agent/scripts/voice_demo.sh` 通过；Python unittest discovery 28/28。
  `voice_demo.sh start` 在缺少 vcan0 时明确 exit 1，未启动 ECU/server/UI。
- 增加 Qt 刷新合同（类型化查询、回执显示、无回答音频、Setup 阶段取消不发送查询），
  Qt 与 stream 合同分别 Debug repeat10 通过。
- Qt offscreen 实际渲染 720/1080/1440 截图成功，产物位于
  `runtime/build/voice-demo/grpc-can-preview-20261006/`；车辆显示未知、音乐未启用。
  这是界面证据，不是模型、WSLg 声音或总线证据。
- UI 输出明确 ready 标记后，在无后端的 offscreen 环境发送 SIGTERM，退出码 0。
  探针等待 ready 而非固定 sleep；动态加载阶段过早发信号曾退出 -15，不视为优雅退出。
  启动脚本也改为等待 UI ready，不能只凭进程存活宣称窗口就绪。
- 独立 `gpt-6.1-sol` / max 只读审查发现 schema 位宽 P2：wire uint32 被 domain uint16
  窄化。已将 Envelope/codec 对齐 uint32，移除 Reactor 临时范围断开；新增
  65537/UINT32_MAX roundtrip 与可信身份关联错误后关闭测试，正常 Setup 成功证明未占容量。
  schema 修复后的 ASan codec / control gRPC / data gRPC / stream 合同 4/4 通过；
  Qt 项仍按上述 LSan 失败记录。Sol max 定向只读复审确认 P2 闭合，未发现新 P1/P2；
  主代理实际执行 Debug 37/37 与相关 ASan 4/4，复审代理未重跑测试。
  TSan 交接、Qt LSan 与真实 vcan 未验证边界仍未关闭。

历史模型质量失败、人工听验缺口、Qt/audio LeakSanitizer 历史发现继续保留；
本轮通信迁移不证明模型质量改善。在线音乐仍等待官方授权和播放器合同验证。

## 实际总线验收及剩余边界

2026-10-07 已实测查询、空调/车窗 ACK、错误事务/帧、ACK 丢失、超时后拒绝变更与显式查询恢复、
提交 gate 拒绝与写后取消、实际 ECU 消失、启动脚本正常/端口冲突失败回收及退出无遗留自有进程。
真实 Qt 刷新与当前证据以次日记录为准，不把测试替身的 Qt 合同冒充实总线验收。
虚拟 ECU 通过仍不是 CAN 硬件、DBC、AAOS 或真实车控部署证据。
