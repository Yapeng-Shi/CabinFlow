# 🚘 CabinFlow

### 离线语音交互，业务无关的 C++ 运行基座。

🌐 [English](README.md) · **简体中文**

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Linux](https://img.shields.io/badge/Linux-WSL2-333333?logo=linux)
![Protobuf](https://img.shields.io/badge/Protocol-Protobuf-4285F4)
![Local inference](https://img.shields.io/badge/Inference-Local_CPU-2E7D32)
![Project stage](https://img.shields.io/badge/Stage-CLI_Integration-orange)

CabinFlow 是基于可复用 C++ Runtime 构建的**离线智能座舱语音 Agent**，
将本地语音识别、意图路由、大模型推理和语音合成连接为可测试的应用流水线。

核心思路是**把系统与模型分开**：通信、任务身份、队列、worker 和取消归 Runtime；
具体算法归 Agent 组件。先搭可运行的系统，再替换或升级模型，不重写整个应用。

> 🚧 项目持续开发中：已形成 x86 CPU / WSL2 / Ubuntu 24.04 上的真实模型 CLI 集成。
> Qt/TCP 交互 Demo 和目标硬件验证尚未完成。

## 💡 为什么做 CabinFlow？

语音助手不只是依次调用模型，还要连接处理阶段、关联任务与结果、限制并发资源、
处理失败并安全退出。CabinFlow 将这些工程基础做成明确、可复用的运行基座。

- **离线推理：** 使用本地模型，不调用云端服务；首次准备依赖和模型需要联网。
- **组件可替换：** ASR、LLM、TTS 使用小型 C++ 接口并通过构造注入，SDK 类型留在适配器内部。
- **执行由 Runtime 管理：** 每 Target 独立有界队列和单 worker，集中处理准入、生命周期与背压。
- **任务可追踪：** Protobuf 消息统一携带 trace/session/work/message 身份、sequence、TTL 与 final。
- **失败与取消明确：** 失败停止下游；取消抑制晚到结果，等待 handler 真正结束再清理。
- **边界可测试：** contract/integration test 覆盖协议、任务隔离、队列、生命周期与组件行为。

当前流水线不增加自动换模型、旧格式探测或通用插件框架。

## 🏗️ 架构

```text
                    Agent：应用与算法

WAV ──→ ASR ──→ Dialogue / Router ──→ LLM ──→ TTS ──→ 回答 WAV
文本 ─────────→         │                     ↑
                        └─ 明确意图 ─────────┘
                           当前返回明确回执，尚未执行车控

                    Runtime：公共执行基座
┌──────────────────────────────────────────────────────────────────┐
│ 消息身份 · work 生命周期 · Ledger · 有界队列                       │
│ 每 Target 的 worker · 取消 · Transport · 可观测性                  │
└──────────────────────────────────────────────────────────────────┘

当前入口：CLI
后续入口：Qt/QML → TCP Gateway → 同一业务流水线
```

Runtime 只看到通用 `Message`，不包含 ASR/LLM/TTS SDK 类型。Agent 节点解析业务
payload，通过注入的 backend 调用算法。同引擎可更换模型路径；换引擎时实现对应
backend 接口，并在启动装配时选择实现。

已有外部 Gateway 使用 **4 字节大端长度前缀 + Protobuf**。内部 Transport 提供
InMemory 和 ZeroMQ 实现；当前模型 Demo 在一个进程中复用 Runtime 执行通道。

## 🛠️ 技术栈

| 层次 | 技术 |
|---|---|
| 系统基础 | C++17、Linux、RAII、多线程、有界队列 |
| 通信 | TCP、epoll Reactor、Protobuf、ZeroMQ |
| 本地推理 | Sherpa-ONNX / ONNX Runtime、llama.cpp |
| 工程验证 | CMake、CTest、contract/integration test、限定范围的 ASan/TSan |
| 后续界面 | Qt/QML；目前不是已验证的界面与播放能力 |

## 📍 当前范围

- **已实现：** 通用 Runtime、类型化 TCP 控制/文本入口、注入式推理适配器和可复用 `VoicePipeline`。
- **已执行：** 真实文本/WAV 经 Runtime 节点处理，生成非静音回答 WAV。
- **下一步：** 真实流水线接 TCP/Qt、播放与用户操作取消，再加 FakeVehicle 空调模拟。
- **不宣称：** 完整语音体验、整体模型准确率、生产车控、AAOS 接入或 RK3576 物理硬件部署。

CLI 当前是单活跃任务、非流式输出，只落盘、不播放。`VoicePipeline::cancel()`
已实现，但 CLI 尚无取消操作入口。模型质量问题与人工听验缺口单独记录，
执行成功不等于语义质量验收通过。

## 📂 项目目录

```text
runtime/                 通用执行、网络、Gateway 与 Transport
agent/
  apps/voice_demo/       真实模型 CLI 与应用装配
  inference/             Backend 接口与原生 SDK 适配器
  pipeline/              可复用语音编排
  dialogue/              类型化文本节点与规则路由
  protocol/              座舱领域 Protobuf
  scripts/               依赖/模型准备与测试脚本
  tests/                 Agent 合同测试与脚本单测
plans/                   实施计划与模块边界
docs/                    仓库级迁移记录
```

旧源码保留为迁移输入，不是生产回退路径。能力状态以
[迁移账本](runtime/docs/capability-migration.md)为准。

## 🚀 开始体验

使用 Linux/WSL，准备 C++17 编译器、CMake/Make、Protobuf 库及 `protoc`、
pkg-config、libzmq 开发文件；依赖准备还需要 curl、tar/bzip2 与 SHA-256 工具。
从仓库根目录执行以下命令，仓库根目录本身不是 CMake 入口。

**不加载模型，先运行 Runtime：**

```bash
cd runtime
./scripts/test.sh
./build/linux-debug/apps/runtime_demo/runtime_demo
cd ..
```

确定性 demo 展示双 session、顺序、过期、final 和取消，不是语音模型演示。
使用 `runtime/scripts/`，不要运行历史依赖安装脚本 `runtime/build.sh`。

**准备并构建真实模型 CLI：**

```bash
bash agent/scripts/prepare_x86_inference.sh
bash agent/scripts/fetch_x86_models.sh
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR="$PWD/runtime/build/inference"
cmake --build runtime/build/voice-demo -j4
```

SDK 与下载的 LLM/TTS 文件是 Git 忽略的本地产物，新克隆不包含这些文件。
脚本锁定来源与校验值。Runtime 构建包含 ZeroMQ 目标，即使 Demo 使用进程内路径
也需要对应开发库。

具体模型路径、文本/WAV 运行命令、结果文件与验证记录见
[真实 CLI 指南](agent/docs/voice-demo-integration.md)。输入必须是完整的
**16 kHz mono PCM16 WAV**，整个编码消息体不能超过 **4 MiB**，每次使用全新输出目录。

## 🧭 后续路线

1. **交互闭环：** TCP/Qt 输入、转写/回答展示与播放。
2. **任务控制：** 可见的取消与清理状态，旧结果不污染新任务。
3. **座舱模拟：** 明确标记的 FakeVehicle 状态与可重复演示用例。

RAG、持续录音/唤醒、多乘员、AAOS 和硬件优化，等首个交互 Demo 可用后再推进。

## 📚 更多资料

- [实施计划](plans/IMPLEMENTATION_PLAN.md) · [目录计划](plans/RUNTIME_DIRECTORY_STRUCTURE.md)
- [Runtime 架构](runtime/docs/architecture.md) · [构建与开发](runtime/docs/development.md)
- [真实组件集成与测试证据](agent/docs/voice-demo-integration.md)
- [模型版本与依赖盘点](agent/docs/model-dependency-audit.md) · [质量与已知失败](agent/docs/demo-model-progress-20261002.md)
- [Runtime 迁移进度](runtime/docs/runtime-migration-closeout.md) · [能力账本](runtime/docs/capability-migration.md)
- [fake 语音 demo](agent/docs/fake-voice-milestone.md)：测试音只验证消息/状态，不是真实推理或回退。
