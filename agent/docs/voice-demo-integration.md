# 真实组件的最小集成切片

更新：2026-10-03。按用户确认，先搭系统并解耦；模型质量独立验收，不阻塞集成。
CLI 与 TCP/Qt 已共用真实组件流水线；窗口、回传和播放器状态已有 x86/WSLg 验证。
FakeVehicle 空调/左前车窗开关及 2.5D 回执驱动界面已接入；CLI 与 Qt 各固定 5×3 次
及真实 Qt/LLM 任务中退出的历史记录保留。最近产品 Debug 33/33、既有 Python 27/27；ASan 2/3，
Qt 音频相关 LSan 本轮首次报 1,672 字节/19 分配、最终复跑 264 字节/3 分配，仍未解决。人工听验、语音语义质量与完整展示尚未通过，
因此不是完整 Demo 验收。

## 边界与替换方式

```text
Qt/QML → TCP Gateway（唯一产品接入）
CLI（复用同一根执行链，用于装配与输入/落盘验证）
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
送入 `cockpit.llm.input`，不是把固定意图回执送给 LLM。明确空调/左前车窗开关意图调用
`agent/vehicle/` 的 FakeVehicle；其他已识别但尚不支持的车辆意图明确失败，
不转给 LLM 猜执行，也不连接真实车辆。

每次任务由 ControlService 创建新 work；同 work 内部跨 Target 复用已批准的规则。
SDK 返回明确结果，失败不进入下游。取消只是请求，编排等待 Runtime completion
确认 handler 实际返回，再清理；不可硬中断的 SDK 不能提前宣称停止。晚到的回答/音频被丢弃。
Qt 提供取消按钮；CLI 暂无取消按钮或信号入口。

## TCP/Qt 任务合同

- Setup 先创建 work；所有外部数据的 target 必须等于该 work 的 unit，内部阶段允许跨 Target。
- 本应用一个 work 接收一条完整 final 输入；非法 payload/final 产生最终失败并清理该 work。
  不做 Ledger 回滚或同 work 重投，下一次输入重新 Setup。
- `cockpit.task.result` 的 `VoiceTaskResult` 显式关联输入 message ID，并携带 transcript、answer
  和唯一 audio/failed/cancelled 结果；独立 `vehicle` 字段保留执行事实。成功 DATA，
  失败/取消 ERROR，不发送成功 ACK。
- Gateway 保存根输入所属连接直到根 completion。Agent 的候选结果不等于完成；根与下游
  handler 都返回后，completion 清理 work、释放单任务槽，再构造一次最终业务结果。
- ExitResponse 仅表示逻辑取消受理，Qt 仍显示“取消中”；收到业务终态才解除忙状态。
  Setup 中取消尚无 handler，成功 Setup 后只发 Exit、不发输入，只等待控制清理。
- Gateway 的四个 `DataTaskHooks` 只交接应用槽位与不透明结果，不能解析 Agent Protobuf。
  未准入产生 DeliveryError；已准入 Runtime 错误统一交给应用 completion 形成 TaskResult。
- `VoicePipeline::shutdown()` 由外部线程先关闭任务准入、取消、等待根清理，再 stop Runtime。
  根在等待下游时直接 stop 会丢掉下游队列、使根永久等待，不能跳过 drain。
- server 的 sigwait 控制线程负责 shutdown，EventLoop owner 负责 Gateway 启停；Qt socket 使用
  事件回调，不在 GUI 线程连接等待或推理。断连显示状态未知，不自动取消或重连。
- WAV 临时文件保留到播放器停止且 source 清空后删除；模型完成和播放状态是两件事。

## Qt 依赖与交互入口

本机 Qt 6.4.2；以下是已安装依赖的合并重现命令（各包已安装，合并命令未另重跑）：

```bash
sudo apt-get install -y --no-install-recommends \
  qt6-base-dev qt6-declarative-dev qt6-multimedia-dev qt6-wayland \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-dialogs qml6-module-qtquick-window qml6-module-qtquick-templates \
  qml6-module-qtqml-workerscript pulseaudio-utils \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-pulseaudio fonts-noto-cjk
```

Qt 依赖只供 Agent 应用前端；`runtime/` 默认构建不需要 Qt 或模型 SDK。
本机首轮 Qt contract 因缺少 GStreamer audio 元件超时；安装所需插件后通过，没有切换
media backend、跳过音频接口或压制错误。首次截图中文缺字，检查 `fc-list :lang=zh`
为空后补齐 Noto CJK；Qt Wayland 插件也已安装。Wayland 测试仍有 Mesa/EGL 驱动警告，
窗口曝光和截图成功，不能据此声称 GPU 加速验证。

按照下节 CMake 命令构建并准备模型后，推荐在 WSL 终端使用统一启停脚本：

```bash
cd /home/projects/CabinFlow
bash agent/scripts/voice_demo.sh start
bash agent/scripts/voice_demo.sh stop
```

`start` 后台启动模型后端，等本次日志中的 TCP ready 标记后打开 WSLg 窗口。
固定使用 `127.0.0.1:39001` 和现有模型，不自动构建或下载。
日志保存在 `runtime/build/voice-demo/launcher/run-*/`；`server.pid`、`ui.pid` 只记录 PID。
`stop` 核验项目二进制和用户后发 SIGTERM：先等后端清理退出，再停 Qt。
关闭窗口不会自动停后端，请再运行 `stop`。每个进程最多等 30 秒，超时保留记录、不强杀。
这是本机开发启停工具，不提供生产进程管理或原子 PID 身份保证；记录损坏、身份不符
或磁盘写入失败时需要人工检查，不要随意删除仍存活进程的 PID 文件。

需要单独调试前后端时，仍可在两个 WSL 终端直接运行同一组二进制：

```bash
cd /home/projects/CabinFlow
runtime/build/voice-demo/voice_demo_server 127.0.0.1 39001 \
  agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 \
  runtime/build/models/qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf \
  runtime/build/models/vits-melo-tts-zh_en
```

```bash
cd /home/projects/CabinFlow
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_demo_ui 127.0.0.1 39001
```

后端命令已实际执行；以下 live probe 用同一 VoiceClient/QML 窗口实际
驱动输入和播放。上述直接 UI 启动命令尚未另外手工操作一轮。服务退出用 Ctrl+C 或
SIGTERM；模型调用若未返回，仍等待清理，不以强制杀进程冒充正常退出。

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
- 历史 CLI/TCP/Qt 阶段的 GPT-6 Astra 独立审查无 P1/P2；这个结果不覆盖本次新增 FakeVehicle。
  GPT-6 Luna 执行有界测试。各阶段实际结果与审查边界分别见下面补记。

SDK 预编译库与既有 llama 静态库没有因此获得 sanitizer 插桩；不声称全模型 ASan/TSan 覆盖。
当前仍是单活跃任务、同步阶段编排、单轮非流式输出；未解决长期历史状态保留，
不声称整个进程长期内存有界。

## 下一步

已批准的座舱可视化扩展见本文末节；步骤 6 的原始结果与手工听验/录屏流程见 [固定用例记录](voice-demo-acceptance-20261003.md)。
CLI 与 Qt 分别运行 15 次；已获准增加只供 C++ 使用的 `answerAudioPath()`，归档同步
复制客户端当前临时 WAV，不改变 QML 或文件所有权。两组转写均为 3/15、空调动作
均为 0/6，原始失败保留。座舱可视化之后继续用户听验、视频及 Qt/audio 泄漏定位，不扩模型诊断或通用 Runtime。
旧错答/ASR 失败保留，暂停诊断扩展；不把 CLI 成功写成完整 Demo、真实车控或 RK3576 部署。

## FakeVehicle 验证补记（2026-10-03）

最小数据流：明确意图 → FakeVehicle set → TTS → 最终 TaskResult → Qt 显示回执。
`VoicePipeline` 拥有唯一模拟状态对象，当前应用构造时显式注入 `FakeVehicle(false, false)`；
没有新增车辆线程、Vehicle Target 或控制框架。Runtime/Gateway 不链接车辆模块。

- `VoiceTaskResult.vehicle` 的字段为 `simulated`、`climate_on`、`action_applied`，与
  audio/failed/cancelled oneof 独立；`action_applied` 表示本任务确实调用 set，
  即使重复设为相同状态也为 true，不代表状态一定变化。
- 动作提交、应用 cancel hook、`VoicePipeline::cancel()`、shutdown 与终态提交共用
  应用任务锁：取消先到则不执行，动作先到则保留事实；TTS 失败、异常、输出超帧也不回滚。
  该原子规则覆盖当前应用取消入口，不扩展为任意调用公开 Runtime API 的通用车控事务。
- receipt 在根 completion 中从独立状态构造，不依赖可能被 SDK 异常替换的候选结果。
  已准入失败/取消仍携带 receipt；未准入的 DeliveryError/忙/本地校验失败不伪造执行事实。
- Qt 收到首份 receipt 前、断连后显示未知；新任务保留“最近回执”并等待本次动作结果。
  旧 work 的晚到 receipt 不覆盖当前状态。失败/取消可以显示已执行事实，但不保留旧回答/音频。

实际验证，WSL Ubuntu-24.04 / x86 CPU，单活跃任务，模型版本/参数保持上文不变：

- 全产品 Debug 构建与 CTest 33/33；Pipeline、TCP、Qt 三项各 Debug repeat20 通过。
  三项 ASan 3/3，无 ASan/LeakSanitizer 报告；未增加 SDK 插桩或新增路径 TSan。
- Pipeline 合同固定动作前/后取消、候选后取消、重复 set、ASR/text 同路径、LLM 文案
  不执行、unsupported intent、TTS 失败/异常/超帧、跨 work 状态保留；TCP 覆盖真实
  socket 的重复动作、失败与动作后取消回执，Qt peer 覆盖缺失/非模拟 receipt 拒绝和旧结果隔离。
- 原生 Wayland/WSLg live probe exit 0：一般文本答北京；WAV 仍转写“法国”，质量未通过；
  再运行“打开/打开/关闭/关闭空调”四条文本，回执依次 on/on/off/off、均 action_applied=true，
  回答明确为“模拟空调已打开/关闭”，每条真实 TTS 均达到 PlayingState 后停止。
  真实一般问题取消完成且没有残留回答/音频；不是动作交错的模型压力测试。
- 本次后端空闲 SIGTERM exit 0；任务中 SDK 所有阶段信号退出仍未验。
- GPT-6.1 Sol 实现整合、GPT-6 Luna 执行指定测试。GPT-6 Astra 因额度失败，新增行为的
  前置独立审查与最终复审尚未完成；未替换模型冒充审查，也不宣布里程碑审查验收。

本次已执行命令（先按上文启动 server，再运行 probe）：

```bash
ctest --test-dir runtime/build/voice-demo --output-on-failure
ctest --test-dir runtime/build/voice-demo \
  -R '^(voice_pipeline_test|voice_gateway_tcp_test|voice_client_test)$' \
  --repeat until-fail:20 --output-on-failure
cmake --build runtime/build/voice-demo-asan \
  --target voice_pipeline_test voice_gateway_tcp_test voice_client_test -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir runtime/build/voice-demo-asan \
  -R '^(voice_pipeline_test|voice_gateway_tcp_test|voice_client_test)$' --output-on-failure
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test --live 39001 \
  /home/projects/CabinFlow/runtime/build/inference/demo-capital-16k-20261002.wav \
  /home/projects/CabinFlow/runtime/build/qt-vehicle-mcpluh
```

Debug repeat20 实际分别运行 Pipeline 单项及 TCP/Qt 两项，上面的合并命令未另重跑。
`qt-vehicle-mcpluh/` 是新建 Git-ignored 本地证据目录，保存 `server.log`、`live.log` 和
`text-ui.png`、`wav-ui.png`、`climate-on-ui.png`、`climate-off-ui.png`，截图已检查中文和
开/关、动作、模拟标记。再次运行必须新建目录，不能覆盖上述产物。
当时播放器启动不是人工听验，5×3 固定用例尚待步骤 6；后续 CLI 与 Qt 分别已留档，
当前语义/人工听验及视频边界见步骤 6 记录，不能用本段历史测试数代替本轮 ASan 结果。

后续补记：本日已用显式 `gpt-6-astra` 独立代理补审 FakeVehicle 与新增测量/runner，
修复 Ctrl+C 的进程组回收 P2 后复审无剩余 P1/P2；上述额度失败是历史状态，不再是当前
审查阻塞。新增 CLI 5×3 次执行、失败和资源记录、Qt 客户端观测计时及一次真实
文本/LLM 任务中 SIGTERM 见 [步骤 6 证据](voice-demo-acceptance-20261003.md)。

## 历史 TCP/Qt 验证补记（FakeVehicle 接入前）

本轮在 WSL Ubuntu-24.04 / x86 CPU，仍使用上述锁定模型、CPU 2 线程、单活跃任务。

- Debug 全产品构建和 CTest：33/33 通过，包括本轮 ZMQ 项。历史 30/31 初次失败仍保留。
- `voice_pipeline_test`、`voice_gateway_tcp_test`、`voice_client_test` 各重复 20 次 Debug 通过；
  三项 ASan 均通过，Qt/TCP 两项最终复测 2/2；没有新增 SDK 插桩、TSan 或 leak suppression。
- TCP test 覆盖实际 socket 的最终音频身份、非法 payload/non-final 终结 work、取消时保持
  busy、两连接结果隔离和新 work；Runtime 入口 test 覆盖外部 owner 拒绝不污染 Ledger，
  同 work 内部跨 Target 仍允许。
- Qt contract 用测试 TCP peer 覆盖 Setup 中取消、取消后准入拒绝、已发 Exit 升级纯清理、
  终态先于旧 Exit、跨 Unit 新任务隔离、错误内层 WorkInfo、断连和 GUI 事件响应。
  这些 peer/模型替身只是合同测试，不是生产回退或真实模型证据。
- Pipeline 测试固定根/下游屏障检查排队取消/过期、候选后取消、异常屏障释放和 shutdown
  等下游；Exit/complete 的 32 轮竞争是有界检查，不保证穷尽或命中所有窄窗口。
- GPT-6.1 Sol 实现/整合、GPT-6 Luna 执行指定测试、GPT-6 Astra 独立审查并复审修复。
  复审范围无剩余 P1/P2；超长身份字段合同仍待用户决定，不作整个协议完整验收。

实际 live probe（不是 offscreen contract）在原生 Wayland/WSLg 窗口中完成文本及 WAV
输入、TCP 最终结果、QMediaPlayer PlayingState 与停止播放。文本仍答北京；WAV 转写
仍只有“法国”，不符合参考问题。播放状态证明播放器启动，不证明人听到了或发音质量正确。

```bash
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test --live 39001 \
  /home/projects/CabinFlow/runtime/build/inference/demo-capital-16k-20261002.wav \
  /home/projects/CabinFlow/runtime/build/qt-probe-RYRhND
```

该目录是本机 Git-ignored 测试证据，截图不可覆盖；独立复现须用新目录。
该 probe 还验证了真实任务“处理中 → 取消中 → 已取消”，没有保留旧回答或音频。
最后一次截图已确认中文正常显示；字体修复前的缺字截图保留，没有覆盖旧证据。
Qt 的定时器在模型处理期间继续触发，证明事件循环有响应，不把计数换算成延迟指标。
后端一次空闲 SIGTERM 退出 0；任务中 shutdown 的确定性等待由 Pipeline 屏障测试覆盖，
不能替代真实 SDK 所有阶段的信号退出验收。上述早期 probe 时 FakeVehicle 与 5×3
仍未完成；后续已接入并留档，当前模型语义、人工听验、视频及长期状态保留仍未完成。

待决上限：外部 message/trace/session/work/source/target/topic 当前没有长度上限；极长
身份重复进入结果体后，最小失败也可能超 4 MiB。Qt 生成的 UUID 不触发此例；已提出
各字段 256 UTF-8 字节的合同供用户确认，尚未擅自实施。

## 历史 CLI 补记（2026-10-02）：WAV 与测试结果

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

## 2.5D 座舱与左前车窗（当前接口，2026-10-03）

沿用上文构建及两个终端的启动命令，默认窗口为 1080×740，最小 720×580。
左侧为驾驶员视角：方向盘、仪表台、左前玻璃及空调气流；右侧保留文本/WAV、
转写/回答、播放/停止和取消。小窗口通过右侧滚动区访问下方播放按钮。

首版只精确识别四条文本；WAV 经 ASR 后复用同一 Router，不直接把文件当作命令：

- `打开空调` / `关闭空调`；
- `打开左前车窗` / `关闭左前车窗`。

泛指“打开车窗”不猜位置；不支持温度、风量、百分比和其他车窗，不接 AAOS/CAN。
运行中的唯一数据流为 Router → FakeVehicle set → TTS → TaskResult → VoiceClient → QML。
Runtime/Gateway 没有新增车辆、领域协议或 Qt 依赖。

`VehicleResult` 当前完整字段为 `simulated`、`climate_on`、`action_applied` 和字段 4
`optional bool left_front_window_open`。生产者在所有已准入终态中写入车窗字段；
消费者先校验存在性，不能把缺字段的默认 false 当成已确认关闭。后端、CLI 与 Qt
须同版重编译；没有旧客户端、三字段 CLI 报告兼容路径，旧报告只保留为历史证据。

FakeVehicle 构造要求显式传入两个初始值，应用为 `FakeVehicle(false, false)`。
同一任务锁下执行动作和取消，每条命令只 set 一个属性；重复开/关不会 toggle。
TTS 失败或动作后的取消仍返回两个实际模拟状态。窗动画约 650 ms，仅为可视过渡，
不表示电机位置/进度；空调气流是展示动画，不是流量测量。未收到回执或断连时
显示未知，处理期间保留最近确认回执，不做乐观车控。

本轮补修了结果校验顺序：关联身份、回执和音频结果先完整验证，再更新可视属性；
无效结果不得通过 changed 回调短暂显示伪造状态。合法回执之后的本地播放失败，
仍保留已经发生的动作事实。独立 Sol max 前/后审发现的两项 P2 均已修复复查。

实际测试与两轮真实模型截图、新增语音失败见
[验收记录第 6 节](voice-demo-acceptance-20261003.md#6-座舱可视化扩展补记)。
中文学习要点：界面绑定回执而非意图；用动作前/后屏障验证取消顺序；Qt 原生绘制
和既有任务锁满足当前展示需求，不引入 3D 引擎或车辆控制框架。
