# 第二周第 1 步：fake 语音消息链

状态：2026-09-29 在 WSL2 Ubuntu 24.04、x86 上实现并验证。此入口是
**fake 演示**，不是生产模型路径，也不是完整可听回答的语音闭环。

从仓库根目录一条命令运行：

```bash
bash agent/scripts/run_fake_voice.sh
```

脚本构建 `fake_voice_demo`，生成固定 16 kHz/单声道/16-bit PCM 静音 WAV，
并将输出写到 `runtime/build/linux-debug/fake_voice_answer.wav`。输出是确定性的
方波测试音，不是人声。fake ASR 只接受这个固定 fixture，并固定给出“打开空调”；
现有 `DialogueTextNode` 只识别座舱意图、不执行车控；fake LLM 固定生成说明文本；
fake TTS 产生测试音。任意真实录音不会被伪装成识别成功。

消息按以下路径依次进入 Runtime 的独立 Target 队列和 worker：

```text
AudioInput / cockpit.audio.input -> asr.primary
TextInput / cockpit.text.input -> dialogue.primary
TextOutput / cockpit.text.output -> llm.fake
SpeechText / cockpit.speech.text -> tts.fake
AudioOutput / cockpit.audio.output -> 演示程序写 WAV
```

测试驱动经 `ControlEnvelopeValidator -> ControlService -> UnitRegistry`
注册 Unit、创建归属 `asr.primary` 的 work，并在完成后显式 Exit。经用户确认，
同一 work 的内部消息可以进入不同 TargetNode；四段 Envelope 保留相同
`trace_id/session_id/work_id`，每条消息使用新 `message_id`。各 topic 是独立流，
首条 `sequence=0`。编排层把最终 `AudioOutput.request_message_id` 关联回原始
音频消息；结束后显式退出并取消 work，断言新数据返回 `work_cancelled`。
演示程序不经过 TCP Gateway，
因此不能用它证明外部客户端首条消息的 target 与所属 Unit 一致性已被强制执行；
这一准入规则还需单独实现和测试。

`runtime/tests/integration/fake_voice_demo_output.cmake` 检查：两次输出 WAV
逐字节一致；四段身份可串联；Exit 后拒绝新数据；缺输入、非固定 WAV 显式失败。
可单独运行：

```bash
cd runtime
cmake --build --preset linux-debug --target fake_voice_demo -j2
ctest --preset linux-debug -R fake_voice_demo_integration --output-on-failure
```

模型文件的固定来源和 SHA-256 已记录在
[`model-dependency-audit.md`](model-dependency-audit.md)。下一步是固定推理库版本、
做真实加载烟测，再逐个替换 fake 节点；真实 ASR、RAG/LLM、TTS，完整阶段
计时、外部 TCP 音频入口和可听回答均未完成。
