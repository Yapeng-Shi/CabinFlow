# Demo 固定用例与展示记录（2026-10-03）

当前结果：系统执行贯通，但语音体验未通过。5 个登记的 synthetic WAV 各运行 3 次，
15 次 CLI 均产出真实回答 WAV；ASR 精确参考匹配 3/15，空调语音动作预期 0/6。
该五条原始证据未改模型、参考、Router 或失败标准。后续 Qt 端也完成独立 5×3 次归档，转写与动作
预期仍失败，见第 5 节。新批准的 2.5D/左前车窗扩展见第 6 节；文本模拟控制已验证，
两条新增车窗语音均漏字，未触发动作。人工听验及操作视频未完成；Qt/audio LSan 未通过。

## 1. 输入、环境和复现

环境为 WSL2 Ubuntu 24.04 / x86_64、Intel Core i7-7700K（8 logical CPUs）。
单活跃任务、顺序运行、每次 CLI 新进程并重新加载模型；OS 文件缓存未控制。
开发工具、contract test/构建与部分运行重叠，因此这是原始观察，不是无干扰 benchmark。

沿用 Sherpa 1.11.3 / ONNX Runtime 1.17.1、llama.cpp b6500
`a7a98e0fffed794396b3fbad4dcdbbc184963645`；ASR int8 bilingual、Qwen3-0.6B Q8_0
非思考、Melo zh_en，CPU 2 threads、LLM context 2048/max new tokens 128。
模型质量与组件执行分别验收，不把项目接入能力等同于上游模型能力。

输入来自已获准生成的 [登记文本](../models/demo-speech-inputs.json)。准备脚本核验登记文件、
源 WAV 和 TTS 许可的 SHA-256，使用 Python 3.12 `audioop.ratecv` 显式离线转换
44.1 kHz → 16 kHz mono PCM16，不改源文件；生产路径仍拒绝不支持格式，不自动转码。
`audioop` 在 Python 3.13 被移除，此准备命令限定 Python 3.12，不自动尝试其他库。

下面命令均已实际执行；复现必须改用从未存在的新输出目录：

```bash
cd /home/projects/CabinFlow
python3 agent/scripts/prepare_voice_demo_cases.py \
  --source-dir runtime/build/inference/demo-speech-20261002-1 \
  --output-dir runtime/build/demo-cases-20261003-1
python3 agent/scripts/run_voice_demo_cases.py \
  --cases runtime/build/demo-cases-20261003-1/cases.json \
  --binary runtime/build/voice-demo/voice_demo \
  --output-dir runtime/build/demo-acceptance-cli-20261003-1 \
  --asr-model agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 \
  --llm-model runtime/build/models/qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf \
  --tts-model runtime/build/models/vits-melo-tts-zh_en
```

准备命令退出 0，runner 退出 1（`failed_observed_contracts`），不是运行器崩溃。
每次保存 `stdout.txt`、`stderr.txt`、GNU time `resources.txt`、`result/result.txt` 和
`result/answer.wav`，总报告为 `report.json`。产物 Git-ignored，不是新克隆自带。
本次原始报告生成后补修了 runner 中断清理和元数据；正常样本未重写，后续运行会额外
记录 runner/binary 指纹及注册版本。不能把当前脚本指纹倒填为旧报告的生成版本。

实际执行二进制 SHA-256：

- CLI：`46fd2bcd0c52469a231c6d288a6132d13c0ebf7e581ba98ac30b4a0a9c803c24`
- Server：`b6652d87d76082d261e2b9b0266bcb44155e827b3d83a5d07dbe51a5e2c203e0`
- GGUF：`9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031`

## 2. 固定语音结果与计时口径

精确匹配只删除 Unicode 标点和空白，不改词语、数字或参考。

| 用例（3 次） | 登记参考 | 本次转写（3 次一致） | 转写匹配 | 回答/动作观察 | CLI 启动至退出秒数（逐次） |
|---|---|---|---|---|---|
| greeting | 你好请介绍一下你自己 | 你介绍一下你自己 | 0/3 | 生成助手介绍，丢了原输入词语 | 18.785 / 17.768 / 17.764 |
| arithmetic | 一加一等于多少 | 一加一等于多少 | 3/3 | 本次回答含 `1 + 1 = 2`；不关闭旧 `/no_think` 原题失败 | 18.214 / 17.513 / 16.810 |
| capital | 法国的首都是什么 | 法国 | 0/3 | 通用法国问询，不是巴黎回答 | 13.104 / 12.447 / 12.949 |
| ac_on | 请打开空调 | 你打开 | 0/3 | 未执行动作，空调仍关 | 11.747 / 11.493 / 11.397 |
| ac_off | 请关闭空调 | 醒关闭声茬 | 0/3 | 未执行动作；当前已关不等于本次关动作成功 | 18.666 / 18.515 / 17.611 |

这些回答由整合方读取真实原始产物，不是 runner 自动语义打分。全部人工听验仍是
`not_verified`，不能把 15 次进程成功换算成语音正确率或用户体验通过。
当前 Router 精确支持“打开空调/关闭空调”，即使未来 ASR 完整转写“请…”也仍有规则
覆盖边界；本次不为变绿而裁掉“请”、重写参考或扩路由。

- Backend 耗时只包住 ASR/LLM/TTS 的实际调用，不含模型加载、排队、日志和序列化。
  返回错误也记录真实 BackendError；SDK 异常没有 completed 计时，不填 0。
- CLI 总时间从 GNU time/CLI 进程启动至退出，包含加载、控制 setup、排队、推理、
  回答文件/报告写入及 shutdown；它不是稳态“说完到听见”的延迟。
- GNU time 记录每次进程的 user/system CPU 秒和最大 RSS（KiB），不是系统 CPU 占用率、
  GPU/NPU 指标或持续服务的常驻内存。15 次最大 RSS 原始范围为 1,488,976–1,605,164 KiB。
- 三次样本只报告原始值，不估计稳定 P95，不与 RK3576/真实车机比较。

## 3. Qt、错误与退出证据

当前真实 Qt/Wayland/WSLg 证据在 `runtime/build/demo-qt-step6-AgnOI9/`：
`server.log`、`live.log`、`shutdown-probe.log` 和四张 text/WAV/空调 on/off 截图。
使用同一生产 VoiceClient/QML、TCP Gateway 与已锁定模型，不是测试 peer 的假回答。

- 文本问北京与 WAV 法国各 1 次；再运行文本开/开/关/关及一般问题取消，probe exit 0。
  WAV 转写失败仍保留，空调只声明文本入口的模拟执行。
- `start_to_local_wav_complete_ns` 从调用 startText/startWav 前，到客户端收到终态并写完
  临时 WAV；含文件读取、Setup、网络、排队、推理与本地文件写入。文本单次 2.155 秒，
  WAV 单次 5.448 秒，不是统计指标。
- `play_command_to_PlayingState_ns` 只观察 play() 到播放器进入 PlayingState，文本/WAV
  分别 401,157/355,329 ns；不是声音出声或到耳朵的时间，不替代听验。
- Server 的 `startup_elapsed_ns=7284614121` 为构造 Pipeline 前至监听 ready，包含
  模型加载、Runtime 装配及 TCP 监听；没有把它冒称纯模型加载时间。
- 任务中 SIGTERM：外部 watcher 核验本次 server PID 对应的可执行文件，等待新输入已进入
  Router handler 后发信号。日志中该 work 的 `llm.primary` 返回 `kCancelled`；Qt 观察到
  `cancelled_before_disconnect=1`，随后未知/no audio/not busy；probe 与服务都 exit 0。
  只覆盖这次文本/LLM 调用，不覆盖所有 ASR/TTS 阶段或全部线程交错。

已执行观察入口（外部控制终端发 SIGTERM，不能从 GUI 杀服务）：

```bash
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test --shutdown 39001
```

错误证据在 `runtime/build/demo-errors-33a8Vx/`：

- 坏 WAV：CLI exit 1，`ASR requires a RIFF/WAVE container`，ASR 返回 `kInvalidInput`，
  无回答/音频，保留模拟 off/no action 回执，不进入 LLM/TTS。
- 缺 GGUF：server exit 1，明确 Missing or empty model file，未监听、不换模型。
- 坏端口：server exit 1，明确 `PORT must be an integer in [0,65535]`。

本轮 Debug 全套 33/33、Pipeline Debug repeat20；Python 最终 26/26；ASan 三项通过。
独立审查发现 runner Ctrl+C 可能留下独立进程组中的模型，已改为 wait 的所有异常
都清理本次拥有的 group、wait 回收后原样抛出，中断报告明确为 interrupted。
真实 timeout 子进程测试及 Ctrl+C/父进程退出/group 不存在的 mock 断言通过；
强制 SIGKILL 清理不是 Runtime 协作取消。GPT-6 Astra 最终源码复审未发现剩余 P1/P2。
SDK 预编译库仍未插桩；不宣称全模型 sanitizer 覆盖。

## 4. 用户手工听验与 2～3 分钟录屏流程（待执行）

按 [运行指南](voice-demo-integration.md#qt-依赖与交互入口)启动一个后端和一个前端。
下面只是操作脚本，不是已有录像或人工通过记录：

1. 0:00–0:15：展示 x86/WSL、离线推理和 FakeVehicle 模拟标记。
2. 0:15–0:50：文本问“中国的首都是哪里？”，等终态、播放/停止，人工判断可懂度与内容。
3. 0:50–1:20：文本开/开/关/关空调，再开/关左前车窗，展示气流/玻璃动画；指出 set 不 toggle、回执为准、非真车控。
4. 1:20–1:50：选择 capital.wav，展示转写缺词与不正确回答，不剪成“语音已通过”。
5. 1:50–2:20：处理中取消，等待实际清理；另在播放中点击取消，确认声音停止。
6. 2:20–2:50：选择坏 WAV 展示明确错误；说明模型质量、听验、硬件与视频边界。

逐次听验可从 `demo-acceptance-cli-20261003-1/<case>-<1..3>/result/answer.wav` 打开回答，
记录是否可听、是否符合完整参考问题、是否存在破音/漏字，不能只看进程退出码。
此处最初只归档 CLI；随后用户继续执行确认 Qt 临时回答 WAV 的只读 getter，并完成
第 5 节的独立 Qt 15 次。软件播放中取消已观察，人工操作录屏与听验仍待完成。

## 5. Qt/TCP 固定用例归档补记

沿用第 1 节设备、模型版本与原始参考；一个常驻后端、一个真实 Qt/QML 客户端，
并发 1，每条创建新 work，模型只启动时加载。没有改模型、Router、协议或 Runtime。
用户“继续执行”确认 `VoiceClient::answerAudioPath()`，不增加 QML 属性、不转移所有权。
GUI 线程取得终态后先同步复制，再泵事件播放；下一任务/断连/关闭仍删除客户端临时
文件，归档副本独立保留。不能把路径字符串的存活当成源文件仍存在。

实际执行核心命令（已有后端；再次运行必须换新目录，父目录应存在）：

```bash
cd /home/projects/CabinFlow
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test --cases 39001 \
  runtime/build/demo-cases-20261003-1/cases.json \
  runtime/build/demo-qt-cases-20261003-zFhzUf/trials
```

本次命令外包 GNU time 并 tee 留日志，exit 1，报告 `failed_observed_contracts`。
15/15 实际完成并复制回答 WAV、保存 Qt 截图、锁存 PlayingState；15/15 在播放器仍
Playing 时调用现有 cancel，观察其停止。这里只验证软件状态，不等于声音已听清。
ASR 3/15 精确匹配、空调动作预期 0/6；一般问答未执行车控，最近回执均为关。
仍为 CLI 中记录的五种转写，关状态不能冒充本次关动作成功。回答原文逐次保存，
人工语义和听验标记始终为 `not_verified`，不将两组结果拼成模型准确率。

| 用例 | 输入开始至 Qt 本地 WAV 完成秒数（3 次） |
|---|---|
| greeting | 20.824 / 18.059 / 18.982 |
| arithmetic | 19.854 / 19.689 / 19.078 |
| capital | 9.452 / 10.515 / 8.956 |
| ac_on | 6.977 / 7.964 / 8.490 |
| ac_off | 21.931 / 20.638 / 22.365 |

时间从 `startWav()` 前到观察业务终态，包括文件读入、Setup/TCP、排队、推理与客户端
临时 WAV 写入，不含启动加载、归档复制、播放和截图。播放计时从 `play()` 前到
changed 回调首次观察 PlayingState，原始值 392054–1071675 ns；不是扬声器首声延迟。
`server.log` 保留 45 条带 trace/session/work/阶段 message/root request 的 backend
调用计时。后端启动至监听为 23094922124 ns，包含加载/装配；与部分测试/构建重叠，
常驻模型批次在有界测试结束后运行。共享开发主机、缓存未控制，不报告稳定 P95。

证据根目录：`runtime/build/demo-qt-cases-20261003-zFhzUf/`（Git-ignored，本机产物）：

- `trials/report.json`：原 manifest/hash、当前测试二进制 hash、Qt 6.4.2/wayland、
  每次参考/回答/回执、实际错误和计时；`<id>-<1..3>/answer.wav` 与 `ui.png` 各 15 份。
- 归档音频 SHA 全部核对；均为 44100 Hz/mono/PCM16，224768–677376 帧，15 份含非零
  样本。容器/非静音检查不等于可懂度。arithmetic 截图已检查中文与模拟标记。
- 输入校验固定仓库注册文件和参考、输入 hash 与声明的 PCM metadata；实际 WAV
  解码验证仍由既有后端负责，不新增解析器或格式回退。
- `host.txt`、`binary-model.sha256`、`server.log`、`qt.log` 与独立 GNU time 文件。
  server 全进程 user/system 486.23/5.92 s、max RSS 1606480 KiB；Qt 280.83/35.15 s、
  max RSS 274964 KiB。这是包含生命周期与 GUI 的累计资源，不是每任务 CPU 或稳定内存。
  batch 后对本次已核实 PID 发 SIGTERM，后端 exit 0，未遗留模型服务。
- 执行 Qt probe SHA：`5b089aae9f22b41103490a6b16522b4bedd712dcee6dd28a2004c4cdc7c1d9eb`；
  server/GGUF 与第 1 节相同。

验证：Debug 全套 33/33，Python 26/26；所有权测试新增成功内容/复制独立性、停止保留、
新任务/取消/断连/关闭/析构删除。错误参数、未知模式、已存在输出目录均 exit 1。
Astra 前审及修复后复审无剩余源码 P1/P2；错误参数误入默认测试和播放错误未计失败
两项 P2 已修正，并保留 PlayingState 事实与播放错误的独立字段。

本轮 ASan 三项首次和复跑均 **2/3**：Pipeline/TCP 通过，Qt test 的 LeakSanitizer
报告 **264 bytes/3 allocations**，栈涉及 libpulse、GStreamer 与 Qt 音频初始化。
尚不能仅凭栈判定第三方根因；没有 suppression、关闭检测或删掉音频测试。
复跑日志在 `runtime/build/qt-archive-checks-lMbwAc/asan-{build,ctest}.log`。
这是当前未解决项，上轮 3/3 是历史范围，不能用它替代本轮结果。人工听验、操作视频
与完整语音体验仍待完成，不标步骤 6 或整体 Demo 最终验收通过。

## 6. 座舱可视化扩展补记

本节为新批准的展示扩展，不覆盖前五节模型质量/旧测试结论。环境和模型沿用第 1 节，
常驻真实后端、Qt 6.4.2/Wayland/WSLg、并发 1；推理无换模型、回退或云端调用。
新增精确 Router 命令“打开左前车窗/关闭左前车窗”和独立模拟属性，Runtime 源码未因
本扩展修改。空调与车窗均按类型化执行回执显示，而非 LLM 的“已打开”文案。

证据根目录 `runtime/build/cockpit-visual-20261003-Hmwtc9/`（Git-ignored、本机产物）：

- `server.log`、`server-resources.txt`；两轮 `live-open.log`、`live-close.log`。
- `live-open/` 9 张、`live-close/` 10 张真实窗口 PNG；包括未知、文本、WAV、空调
  on/off、车窗 open/closed、组合 on/open、最小尺寸，第二轮还包括滚动后的播放按钮。
- 默认 1080×740 和最小 720×580 均曝光截图；已检查中文、模拟标记、未知与两个独立
  状态。最小尺寸的播放按钮需滚动，probe 实际滚动并断言按钮落在可视区域。
- 每轮真实文本开/开/关/关空调、开/开/关/关左前车窗均有真实 TTS、PlayingState 和
  停止；最后再开空调/车窗验证组合状态。每轮 10 次车控均为模拟，不是真车动作。
- 一般文本答北京；每轮处理中取消等待 handler 清理，未残留回答/音频。动画终点
  由 probe 等待 `glassLevel` 接近 0/1 验证，截图不是伪造渲染或生成图片。
- 第一轮从后端两个属性皆关开始；第一轮结束保留 on/open，第二轮一般问答回执仍为
  on/open 且 action 未执行。这是跨任务/连接状态，不是问答触发了车控。

新增语音在执行前固定参考，用获准的同一 Melo TTS 生成，离线明确转换 44.1→16 kHz
mono PCM16。`synthetic-window-inputs.json` 记录参考、许可 hash、源/输入 PCM 元数据
和 hash；不改原五条验收集，人工听验仍 `not_verified`。每条只运行一次，不估计准确率：

| 固定参考 | 输入 SHA-256 | 实际 ASR | 本次动作 |
|---|---|---|---|
| 打开左前车窗 | `068522d66571a9c315845dace665db273c8dd6e83ed2c7ce9752cb04454e08ba` | 打开左前车 | 未执行，仍为关 |
| 关闭左前车窗 | `f192fbb78d963e9ec35af7900b2c6386af98d7cf54c82f7e01ed0d50ac6fbdf0` | 关闭左前车 | 未执行，保留此前全开 |

两条均未精确识别，因此通用 LLM 回答并生成真实音频；不通过删字/补字/扩大 Router
把失败改成成功。文本控制可展示，语音车窗控制质量没有通过。

核心命令已执行，复现时必须换新的证据目录并预先准备相应 16 kHz WAV：

```bash
cd /home/projects/CabinFlow
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test --live 39001 \
  /home/projects/CabinFlow/runtime/build/cockpit-visual-20261003-Hmwtc9/window_close/input16k.wav \
  /home/projects/CabinFlow/runtime/build/cockpit-visual-20261003-Hmwtc9/live-close
```

两轮 probe 均 exit 0；这只表示执行、回执、播放器和展示合同通过，probe 不验收 ASR
语义；上述参考对照单独判为失败。关窗语音事实由日志及 WAV 截图核验。模型启动到监听
`startup_elapsed_ns=11356958463`，含加载/装配/TCP；与开发测试重叠，不作性能统计。
本轮 server SHA `198b137a80aeba3645981b82fcdd19ff468d19cacdb8217d591d076d108787ea`；
第二轮 probe SHA `ec80b06c0f69a821d89fafd4e42ca8d39f729fd87377d2c9ffeb141e21da3468`。
第一轮后新增只读日志/最小尺寸滚动断言，故不把第二轮指纹倒填成第一轮版本。

验证：全产品 Debug 最终 33/33，Python 27/27；Pipeline/TCP/Qt 各 Debug repeat10
通过，之后增加只读 UI 定位/滚动断言并重新构建 UI/probe、全套 33/33 通过。
Pipeline 与 TCP 的 ASan 通过；Qt LSan 首次仍失败，**1,672 bytes/19 allocations**，
栈涉及 libpulse/GStreamer/Qt Multimedia。原始 CTest 日志已保留为 `asan-last-test.log`；
最终 UI/probe 重编译后单独复跑 Qt 仍失败，264 bytes/3 allocations，记录在
`asan-qt-final.log`；两次泄漏规模不同，未据此断言测试时机或第三方就是根因。
不使用 suppression，不关闭 leak detection，也不仅凭栈把根因归给第三方。
预编译模型 SDK 未插桩，本扩展未运行新的 TSan，不声称整体 sanitizer 通过。

实际工作分工：Sol 实现/整合；独立显式 `gpt-6.1-sol` / `max` 前审、后审及修复复查
无剩余 P1/P2；`gpt-6-luna` / `low` 执行 repeat10 与有界 ASan，首次 WSL 转义失败
未计通过，主 agent 重新构建和跑全套后再补跑。没有调用 Astra。视觉 QA 和最后
只读定位/日志/滚动测试由主 agent 检查；绿色测试不是全部线程交错或语音体验证明。
两轮结束后核验本次 server PID 的 `/proc/<pid>/exe`，只对该服务发送 SIGTERM，
等待前台进程退出 0，确认未遗留本次模型服务；这次是空闲退出，不新增所有 SDK
阶段的任务中信号测试结论。

剩余仍是人工听验、操作视频、ASR/模型质量与 Qt/audio LSan 定位；录音/VAD、真实
车控、AAOS、CAN、RK3576 均未新增。界面使用原生 Qt Quick 绘制，动画不代表电机
进度，硬件部署与完整 Demo 最终验收仍未完成。
