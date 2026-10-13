# 本地唤醒与单轮录音：实现及验收边界

## 当前结论

2026-10-13 用户确认“你好，小舱”本地唤醒，以及 5 秒等待开口、Silero VAD 静音 0.8 秒结束、15 秒总录音上限。代码接入不是唤醒质量通过：当前合成输入没有检出该词，本人录音、真实麦克风采集和完整交互仍待验证。麦克风默认关闭，界面明确标为“待真人验收”。

用户随后确认“先打通”，不等录音作为系统集成前置条件，且明确拒绝直接录音按钮。测试端口的录音交接 → **真实 gRPC/ASR/Router/RAG/LLM/TTS → 回答 WAV/WSLg PlayingState → 前端和服务退出**已跑通；这不是实际 KWS/麦克风成功。生产不增加强制唤醒、直接录音或自动回退入口。

## 数据和所有权

```text
用户显式开启麦克风
  → 客户端 QThread：QAudioSource → 16k mono PCM16 → KWS
  → 显示“请说指令”（先唤醒，看到提示再说；不承诺一口气下令）
  → 同一 worker：Silero VAD → 一段完整 WAV → 停采音
  → GUI 校验 epoch / 录音所有权 → 原 Setup / AudioInput
  → gRPC → 既有 ASR / Router / RAG / LLM / TTS
  → 原结果与手动回答播放
```

- QML 只展示状态。采音对象、模型、PCM 缓冲和计时器在 worker 构造、使用和销毁；GUI 不推理、不等线程 join。
- Runtime、Gateway 不新增唤醒/录音业务接口。唯一 WAV 编码器从 NativeBackends 拆为小 AudioCodec，客户端不因此链接 Python/llama 后端。
- `busy_` 仍只表示后端任务；界面忙还包含录音、准备和停止中。录音不是已创建 work，取消录音不能向旧 work 发 Exit。
- GUI 为每次监听分配 epoch；停止时先使 epoch 失效，再请求 worker 清理。迟到 awake/WAV/error/settled 不能提交、释放新任务的锁或重启监听。
- 录音持有原 MusicController 语音 reservation，完成 WAV 时直接交给原任务，不先释放再争抢。调试文本/WAV、车辆刷新也先等麦克风停下再提交，不积压输入。
- PCM 字节可在半个16位样本处分片，只解码完整512样本帧。应用检查排队总字节不超过32,000（1秒）；VAD缓冲16秒，正常指令不超过15秒/480,000 PCM字节。
- 样本数与单调 wall-clock 双重限制；在推理前、推理后和 WAV 编码后检查期限，不能等事件循环 timer 才发现超时。5秒未开口和15秒超限取消本轮，不 Flush、不填静音、不发送截断片段。连续2秒无设备数据是输入故障，停止监听。
- 回答播放意图从接受播放请求保持到停止/失败/结束，覆盖降音等待和播放器开始前的空隙；等待停止/降音时也可按“停止播放”。任务、音乐变更、播放/恢复音量、断连和关闭时暂停监听，不提供播报打断。
- 正常退出先停采音、销毁模型，worker 再退出事件循环；`closeReady` 必须等实际 QThread finished、音乐与 gRPC 清理，不能把 stop acknowledgement 当成线程已销毁。

## 固定依赖与失败记录

已有 Sherpa **1.11.3** / ONNX Runtime **1.17.1**、Qt **6.4.2** 不升级，原 ASR/LLM/TTS 不换。

新 KWS：`sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01`，epoch12/chunk16/left64 int8 encoder/decoder/joiner，单线程 CPU，boost1、threshold0.25、trailing blank1。模型包 README 声明 Apache2.0；[上游模型和自定义词说明](https://k2-fsa.github.io/sherpa/onnx/kws/pretrained_models/index.html)。

VAD：上游固定 **Silero v5.0** 文件，512样本窗口，threshold0.5、minimum speech0.25秒、minimum silence0.8秒；[官方模型接入说明](https://k2-fsa.github.io/sherpa/onnx/vad/silero-vad.html)及[MIT许可](https://github.com/snakers4/silero-vad/blob/master/LICENSE)。SDK的 `max_speech_duration` 设60秒，防止它在应用15秒边界前改变阈值/静音条件。模型文件 SHA-256 统一在 `agent/models/wake-models.sha256`，进入可能 `exit()` 的 SDK 前再校验，损坏文件明确报错。

首次下载未固定版本的 k2-fsa `silero_vad.onnx` 在现有 SDK 报 `Unsupported silero vad model`（诊断进程exit1）。未升级 SDK，也没有生产自动回退；最终只使用固定v5.0，旧失败文件仅在忽略的本地诊断目录中保留。

独立合成输入保持“你好，小舱”“请打开空调”“今天天气怎么样”原文。固定 MeloTTS/speaker0，速度1及1.5的实验目录分别保留；测试准备复用既有 Python3.12 `audioop.ratecv` 做44.1k→16k转换，明确追加一秒安静作为流输入尾部，不进入生产算法。原五条语音及参考/hash不改。

本程序 `wake_engine_test` 的正例断言失败；同版本官方 KWS CLI 对同一音频也未检出。用同一小模型自由解码，速度1输入得到 `nǐhǎodà`，不是目标词；仅凭此不能断言是模型还是合成发音问题。一次 boost3 对照仍失败，已恢复boost1；不继续盲目调阈值、换词或扩大模型诊断。

## 验证与未验证

本轮环境：i7-7700K、x86 WSL2 Ubuntu24.04；不是车机/RK3576验证。

- Debug CTest：41/41工程测试（不包含下方独立真实KWS门槛），配置的RAG Python环境全套32/32。新增 PCM 分片/溢出、真实worker缺失/损坏模型与退出、fake采音+真实gRPC关联、重复/旧代结果、取消/关闭、音乐操作/降音恢复/迟到播放、致命协议错误重入测试。
- 限定 ASan：PCM/worker/VoiceClient3/3（Qt测试明确 `detect_leaks=0`）；独立纯PCM LSan1/1。新构建首次误选Windows Anaconda的re2，清除该缓存并明确排除其包目录后使用已安装Linux pkg-config RE2重跑，不升级库、不修改系统PATH。
- 真实输入模型验收：**未通过**。引擎测试在KWS正例处停止，因此它后面的VAD静音间隔、连续PCM上限、推理跨期限和正常WAV完成断言不能算通过。代码审查不替代这些测试；等本人录音复跑。
- 设备只枚举到 `RDP Source`，支持16k mono PCM16；枚举不等于实际捕获。测试不自动打开真实麦克风。
- 原 ASR质量问题、Qt LSan和gRPC TSan仍未解决；本轮不把它们改为通过。
- Qt界面可渲染，1440/1080/720截图仅验证布局；测试采音替身不是实际唤醒。
- 新录音交接的真实后端烟测：`voice_client_test --wake-chain` 使用原登记 `arithmetic.wav`（SHA-256 `87ccd9fb630f3daac2fbe42b30aa0f9c12ce9b02883d6cffcb845af7222f2cf5`），明确注入测试awake/WAV，不运行KWS/麦克风；真实后端转写“一加一等于多少”，返回真实回答音频，WSLg到达PlayingState，前端正常关闭。单次集成结果不覆盖历史语义错误，不当作准确率或性能基准。
- 统一启动脚本两次start/stop通过（麦克风默认关闭），停止后没有自有UI/后端/ECU遗留。本轮未宣称任何实际采音。

Sol max独立审查修正了推理跨过wall-clock期限、致命错误通知前监听重启、测试回调线程上下文三个问题；修正后聚焦复审未发现额外P1/P2。跨期限/短静音/连续PCM的引擎断言已编写，但因KWS正例失败尚未执行到这些断言，不能把代码审查算作其通过。

最终全套Debug记录在 `runtime/build/voice-demo/wake-delivery-ctest.log`，前序日志为 `wake-accepted-*`；最终真实KWS正例仍失败，见 `wake-engine-delivery.log`，历史失败在 `wake-engine-test-v3/v4/v5/v6.log`、`wake-upstream.log`、`wake-free-decode.log`。它们与模型/语料均是 Git忽略的本地产物。
真实接线报告为 `runtime/build/voice-demo/wake-chain-20261013/report.json`、`answer.wav`、`wake-chain-ui.png`；ASan记录在 `runtime/build/wake-asan-20261013/`。

## 重现与下一步

先准备原推理环境、RAG及UP的vcan0，再执行：

```bash
cd /home/projects/CabinFlow
bash agent/scripts/prepare_wake_models.sh
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR=/home/projects/CabinFlow/runtime/build/inference
cmake --build runtime/build/voice-demo -j2
ctest --test-dir runtime/build/voice-demo --output-on-failure
bash agent/scripts/voice_demo.sh start
# 界面开启“本地唤醒 · 麦克风”，先说唤醒词，看到提示再说指令。
bash agent/scripts/voice_demo.sh stop
```

独立KWS验收（以下命令已执行，正例未通过，不能列作成功演示）：

```bash
python3 agent/scripts/generate_wake_inputs.py --output-dir /绝对路径/新的语料目录
runtime/build/voice-demo/frontend/wake_engine_test \
  /home/projects/CabinFlow/runtime/build/models/wake \
  /绝对路径/语料目录 /绝对路径/新的证据目录
```

下一步需本人录制“你好，小舱”（停顿约1秒）后“请打开空调”的WAV，并提供独立唤醒/指令样本进行模型及VAD复验，然后在WSLg明确启用麦克风验证完整链、停止和无遗留进程。人工听验、无唤醒误触发率、噪声/远场、AEC、持续对话和流式传输均未完成；不据此写“可用车机语音唤醒”。

该录音需求是后续质量/设备验收，不再阻塞系统接线。测试烟测命令（只供测试二进制，非生产假唤醒入口；需先启动统一脚本）：

```bash
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test \
  --wake-chain 39001 /home/projects/CabinFlow/runtime/build/demo-cases-20261003-1/arithmetic.wav \
  /绝对路径/新的接线证据目录
```
