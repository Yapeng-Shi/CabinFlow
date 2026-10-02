# 真实组件的最小集成切片

更新：2026-10-02。按用户确认，先搭系统并解耦；模型质量独立验收，不阻塞集成。
当前是 CLI 切片，不是已经完成 Qt/TCP 的交互 Demo。

## 边界与替换方式

```text
CLI（装配与输入/落盘）
  → VoicePipeline（任务与阶段编排）
  → Runtime（每 Target 独立有界队列、worker、Ledger）
  → Agent TargetNode → AsrBackend / LlmBackend / TtsBackend
                         ↑ 启动时注入 Sherpa / llama.cpp 具体适配器
```

- `agent/inference/` 定义最小端口；原生 SDK 仅在具体适配器源文件中使用，Pimpl 隐藏资源类型。
- `agent/pipeline/` 是 CLI/后续产品入口可复用的编排，不包含具体模型 SDK。
- Runtime 不链接 Agent 模型库，Gateway 不解析领域模型 payload；Runtime 原构建入口不变。
- 更换同引擎模型：在启动装配时指定新模型；更换引擎：实现对应端口并更换构造注入。
  不重写 Runtime 或整条链，不增加插件、工厂注册、运行时失败回退。
- 固定节点身份：`asr.primary`、`dialogue.primary`、`llm.primary`、`tts.primary`，不绑定模型名。

WAV 先进入 ASR；明确的文本入口跳过 ASR。Router 保留原问题，未识别意图才把原文
送入 `cockpit.llm.input`，不是把固定意图回执送给 LLM。明确意图暂返回“未执行车控”
的真实回执，没有实现 FakeVehicle，不能宣称已经打开空调。

每次任务由 ControlService 创建新 work；同 work 内部跨 Target 复用已批准的规则。
SDK 返回明确结果，失败不进入下游。取消只是请求，编排等待 Runtime completion
确认 handler 实际返回，再清理；不可硬中断的 SDK 不能提前宣称停止。晚到的回答/音频被丢弃。
`VoicePipeline::cancel()` 已实现，CLI 暂无取消按钮或信号入口。

## 构建与运行

已实际配置/构建（模型与 SDK 已在本机按既有记录准备，不是新克隆自带）：

```bash
cd /home/projects/CabinFlow
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR=/home/projects/CabinFlow/runtime/build/inference
cmake --build runtime/build/voice-demo -j4
```

本轮实际文本演示命令；再次运行必须换一个从未存在的输出目录：

```bash
runtime/build/voice-demo/voice_demo --text '中国的首都是哪里？' \
  runtime/build/inference/voice-demo-text-20261002-2 \
  agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 \
  runtime/build/models/qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf \
  runtime/build/models/vits-melo-tts-zh_en
```

该次退出 0，实际答案为 `中国的首都是**北京**。`。`result.txt` 保存 trace/session/work
和输入 message ID；`answer.wav` 为 44.1 kHz、mono PCM16，70,144 帧，非零样本。
SHA-256：`3ae7b502263d131189eeb9ccccf7a52e1b42361e462f06089c36e6dac172e950`。
这是单条执行与答案证据，未完成人工听验，不推导整体正确率或延迟指标。
之前测试包装误建输出目录的失败及日志保留，未覆盖。

WAV 入口把 `--text TEXT` 换成 `--wav INPUT.wav`，其余参数相同。
只接受完整的 16 kHz mono PCM16 WAV，不在运行时猜格式、自动重采样或改用文本。
整个 Protobuf 编码帧体须不超过 4 MiB，不能只看 WAV 文件大小。
TTS 输出保留实际采样率，输入音频与回答音频不要求同一采样率。

## 当前依赖与验证口径

- WSL Ubuntu-24.04 / x86 CPU；Sherpa 1.11.3 shared、ONNX Runtime 1.17.1、llama.cpp b6500。
- 沿用现有 ASR int8、Qwen3-0.6B Q8_0 非思考、Melo zh_en；没有切换模型。
- 当前 CLI 的 llama 参数是 CPU 2 线程、context 2048、最多 128 新 token；达到上限但
  未正常结束是显式失败，不拼接假答案。这不是此前 context 512/16-token 算术烟测的重跑。
- 测试替身仅存在于 unit test；不作为生产推理或自动回退。
- 新增 `voice_pipeline_test`：原文传递、独立 work、失败中止、明确意图、LLM/TTS 取消、
  清理期间拒绝新任务、晚到输出抑制、输入/输出编码帧上限。
- 新增 `wav_contract_test`：PCM16 往返、截断、声道/采样率、异常块长度、重复 fmt、空/静音/NaN。
- GPT-6 Astra 已独立审查生命周期、取消、依赖边界及新增测试；无 P1/P2 发现。
  GPT-6 Luna 执行有界测试与真实 CLI。实际测试结果在下面的本轮补记中记录。

SDK 预编译库与既有 llama 静态库没有因此获得 sanitizer 插桩；不声称全模型 ASan/TSan 覆盖。
当前仍是单活跃任务、同步阶段编排、单轮非流式输出；未解决长期历史状态保留，
不声称整个进程长期内存有界。

## 下一步

沿计划接唯一 TCP/Qt 产品路径：先确定现有 Protobuf 下最终音频、取消清理通知和关联
保留合同，再装配 Gateway 与这一编排，验证 WSLg 窗口/播放。随后才加 FakeVehicle。
旧错答/ASR 失败保留，暂停诊断扩展；不把 CLI 成功写成完整 Demo、真实车控或 RK3576 部署。

## 本轮补记：WAV 与测试结果

真实 WAV 路径已单次执行：`demo-speech-20261002-1/capital/tts.wav` 是用户获准生成的
44.1k fixture，参考“法国的首都是什么”。测试准备使用 Python 3.12 标准库
`wave/audioop.ratecv` 显式离线转换为 `demo-capital-16k-20261002.wav`，16k mono PCM16，
24,335 帧；没有在 Runtime 加自动转换，没有覆盖旧输入。
源 SHA-256：`6e8a2a0bf4a885f6e41952faa74823f4027908d20db73e39606188ee948730de`；
新 SHA-256：`2b3628b2061e4f217a47835c7af1fb9ca998df5a6b770a6c68a15190e6318c09`。

```bash
runtime/build/voice-demo/voice_demo --wav runtime/build/inference/demo-capital-16k-20261002.wav \
  runtime/build/inference/voice-demo-wav-20261002-1 \
  agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 \
  runtime/build/models/qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf \
  runtime/build/models/vits-melo-tts-zh_en
```

该命令退出 0，但 ASR 转写只有“法国”，LLM 生成通用法国问询回答，不是参考问题的
正确答案。执行贯通与问题回答正确是两项不同验收；后者未通过，不继续调参。
输出 WAV 为 44.1k mono PCM16，311,296 帧，非零样本，SHA-256：
`2f1a344eec0c2afeb6b88296b4c8b53912b5ebcd87aa353748682690dc3e3776`；没有人工听验。
所有产物在 Git 忽略的 `runtime/build/inference/`，不是新克隆自带数据。

已执行验证：

```bash
ctest --test-dir runtime/build/voice-demo \
  -R '^(voice_pipeline_test|wav_contract_test)$' --repeat until-fail:20 --output-on-failure
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo-asan \
  -DCMAKE_BUILD_TYPE=Debug -DCABINFLOW_ENABLE_ASAN=ON \
  -DCABINFLOW_INFERENCE_DIR=/home/projects/CabinFlow/runtime/build/inference
cmake --build runtime/build/voice-demo-asan --target voice_pipeline_test wav_contract_test -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir runtime/build/voice-demo-asan \
  -R '^(voice_pipeline_test|wav_contract_test)$' --output-on-failure
```

新增两项 Debug 各重复 20 次通过，ASan 2/2 通过，无模型语义或第三方 SDK 全覆盖声明。
完整产品构建通过，但全套 Debug 首次 30/31：`zmq_transport_test` 报
`expectation failed: message delivered`。随后单独诊断该项退出 0，未改断言或生产实现，
没有把初次失败抹掉，暂不宣称完整套件稳定通过。本次未执行新增路径 TSan。
`git diff --check` 通过。最终 TTS 取消覆盖“先取消再释放 SDK”的确定顺序，不是穷尽线程交错。
