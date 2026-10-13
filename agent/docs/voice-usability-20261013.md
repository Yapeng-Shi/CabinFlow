# 语音可用性完善：意图合同对齐（2026-10-13）

## 问题、原因和修正

原登记语料 `agent/models/demo-speech-inputs.json` 的空调参考是“请打开空调”“请关闭空调”，合成文本还带中文句号。Router 原来只接受“打开空调”“关闭空调”。即使 ASR 给出完整正确参考，也会进入 RAG/LLM 而不是明确车控。

检查时真实 CLI 输入“请打开空调”退出 1，未执行车控，最终报 `Llama reached max_new_tokens before end of generation`。旧记录在 `runtime/build/intent-contract-20261013-ibFkSc/`，不覆盖。

本步只增加四个完整字符串匹配。没有通用去前缀/标点、子串、模糊匹配或模型猜测车控；window/seat/music 规则不变。原文仍完整保留在 transcript，生命周期/取消/消息关联和 CAN 执行链不改变。

测试以原登记 JSON 的 `reference_text` / `text` 为输入源，不另抄“能通过”的句子；两个 AC id 必须唯一且同时出现，字段必须为非空字符串。测试中的 JSON 只读取验收元数据，不是生产消息解析器。额外验证否定、问句、引述、条件和多动作句的 `action_applied=false` 与车况不变，避免仅检查幂等状态而漏掉误执行。

## 实际验证

环境仍为 x86 / i7-7700K / Ubuntu 24.04 / WSL，Sherpa 1.11.3、ONNX Runtime 1.17.1、Qwen3-0.6B Q8_0 非思考、MeloTTS；没有换模型或推理策略。车辆仅使用 vcan0 与独立虚拟 ECU。

| 检查 | 结果和范围 |
|---|---|
| 修正前新增回归 | Pipeline/gRPC 0/2，正确暴露礼貌表达误入 RAG/LLM |
| 修正后完整 Debug | 39/39，构建通过 |
| 限定 ASan/LSan | `voice_pipeline_test` 1/1；使用明确测试替身，不覆盖 Qt/Torch sanitizer |
| 原参考输入合同 | 两个 AC case 的 reference/text，加两次“完美转写”ASR 端口替身均执行正确，RAG/LLM 零调用；不是实际 ASR 质量验收 |
| 非指令保护 | 12 个否定/问句/引述/多动作输入保持零动作，LLM 即使声称执行也不影响车辆 |
| gRPC | 四种新表达通过 typed final / trace/session/work/request 关联、正确回执、无成功 ACK 和 RAG/LLM 绕过断言 |
| 真实模型 CLI/vcan | 四种表达均退出 0，收到 on/on/off/off 与 `action_applied=1`，真实 TTS 生成 WAV，没有 RAG/LLM backend call |
| 独立审查 | 显式 `gpt-6.1-sol` max 只读设计/实现审查无阻断项；不作为无缺陷证明 |

Debug/ASan 日志：`runtime/build/voice-intent-20261013-eYxAy2/`。真实 CLI/vcan 的日志与 WAV：`runtime/build/voice-intent-live-20261013-UcymTM/`。测试自有 ECU 已退出。

原登记输入 hash 保持 `05bc6c7330e027397e9141dab4479c370e8344bddcf7fcd9181a7155e43874c5`，未修改原 WAV、许可说明或参考文字。

可重复的自动回归命令：

```bash
cd /home/projects/CabinFlow
cmake --build runtime/build/voice-demo -j4
ctest --test-dir runtime/build/voice-demo --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir runtime/build/voice-demo-asan \
  -R '^voice_pipeline_test$' --output-on-failure
```

本轮已执行；ASan 源码变化后须先重建对应 target。真实 CLI 使用当前必填 RAG 三参数，即使该条指令绕过检索，也不启用缺模型的假后端。

## ASR 的独立隔离检查

固定原五条获准生成的中文语音；每条分别调用同一 Sherpa 1.11.3 上游 CLI 识别原 44.1kHz 与既有准备后的 16kHz WAV，共 10 次独立进程，CPU 2 threads、greedy、并发 1。此处上游 CLI 对原采样率的处理只用于诊断，生产 ASR 仍只接收明确的 16kHz mono PCM16。

| 原输入 | 上游 CLI：44.1kHz / 16kHz |
|---|---|
| 你好，请介绍一下你自己。 | 你介绍 / 你介绍一下你自己 |
| 一加一等于多少？ | 一加一等于多少 / 一加一等于多少 |
| 法国的首都是什么？ | 法国 / 法国 |
| 请打开空调。 | 你打 / 你打开 |
| 请关闭空调。 | 醒关闭声茬 / 醒关闭声茬 |

只有算术输入的两种版本匹配参考；诊断 gate 正确退出 1。日志、每次完整命令/输入 hash 和报告在 `runtime/build/asr-isolation-20261013-OzKEav/`。不以正常退出或文本修正代替识别正确。

这证明转写问题可以脱离 Qt/gRPC/Runtime 在上游 CLI 复现，调查可聚焦输入音频与识别后端；不能据此断定一定是 ASR 权重、合成音或某个采样率处理的单一根因。人工听验与真人录音对照仍待完成。

## 后续边界

本步不是完整语音验收，也没有实现麦克风、唤醒、免唤醒多轮、流式 ASR/LLM/TTS 或播报打断。现有 gRPC TSan / Qt LSan 失败状态保持；没有提交或推送 Git。

用户提出关键词唤醒后，正在确认本地 KWS 主入口与同一采集/VoicePipeline 的设计；现有 Sherpa SDK 提供 KWS C API，不等于当前 ASR 模型已验证可用作唤醒模型。模型、唤醒词和监听策略确认前，不下载或开启常驻录音。
