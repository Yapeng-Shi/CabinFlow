# 座舱全景与音乐控制：2026-10-04

## 当前交付边界

前排全景和音乐控制层已实现；**真实网易云音乐尚未启用**。生产装配没有 MusicBackend，
中控明确显示“在线音乐未启用”，不展示测试歌曲、不伪造播放，不存在备用音乐源。
ASR/LLM/TTS 继续本地运行；后续网易云搜索/授权/播放是显式联网功能，不能称整个应用全离线。

| 阶段 | 状态 |
|---|---|
| ① 参考图布局 | Qt/QML 2.5D 前排全景、音乐中控、独立语音面板；实际渲染截图已生成 |
| ② 类型化控制 | Router/Pipeline/TCP/Qt 控制、任务互斥及音量事务已实现；用测试替身验行为 |
| ③ 官方接入 gate | 待开发者授权；本次 PATH 未找到 node/npm/ncm-cli/mpv，未安装、未登录、未实测 |
| ④ 真实播放与听验 | 未开始；QProcess CLI 适配器、两首真实歌曲、真实降音和 mpv 回收未验收 |

无 Runtime Core/Gateway 业务扩展；仅更新一个链接 Agent 的旧集成测试以接纳音乐意图。
空调/左前窗仍按类型化回执展示，不接真实车辆，不修改模型、原语音参考或旧失败结论。

## 数据与所有权

```text
文本 / WAV → 同一个 ASR / Router → MusicCommand（跳过 LLM/TTS）
→ VoiceTaskResult.music_command = 8 → 现有 TCP → VoiceClient → MusicController
                                                        └─ 真实 Backend 未装配
```

- MusicCommand 表达 SEARCH、PLAY、PAUSE、PREVIOUS、NEXT、SELECT。keyword/result_index
  只允许对应命令使用，非法字段组合显式失败，不交给 LLM 猜执行。
- 固定规则：`搜索周杰伦`、`播放音乐`/`继续播放`、`暂停音乐`、`上一首`、`下一首`、
  `播放第一首`～`播放第十首`及阿拉伯数字编号；QML 没有第二套关键词解析。
- 音乐终态表示“识别完成，待播放器执行”，不是音乐成功。CLI 诊断仅记录指令和
  `music_execution=not_executed_by_cli_diagnostic`，不写空 answer.wav。
- 控制器拥有 Backend，VoiceClient 借用且先于控制器销毁。Backend 是内部类型化异步端口，
  **不是虚构的网易云 JSON/终端格式**；生产没有测试替身或“开启音乐”配置开关。
- 每次异步请求带 operation_id；ACK 后还须回读歌曲身份、状态和音量。旧回调不更新新操作。
  语音提交前锁列表，终态一次性消费后交接锁，才通知 UI；取消先提交标记再发可重入信号。
- 新搜索立即清空旧编号，失败不能按新编号播放旧结果；冻结的播放 playlist 独立保留。
  最多十项，不自动播放、循环或跳过不可播歌曲；歌曲 ID 保持字符串。
- 操作有 20 秒期限；超时只发一次 abort，等待 quiescent 证明旧副作用静止后释放槽。
  空闲时每秒读播放状态；失败后停止自动查询。EOF 显示结束，不自动切歌。
- 普通回答仍手动播放；控制层要求 Backend 存在时记录实际基线，降至 20% 并确认后才播 TTS。
  停止使旧播放 generation 失效，但旧音量写入仍保有操作槽直到恢复完成；恢复不 resume、不重试。
  回答播放中可搜索/暂停；切换/恢复音乐需先停回答。音量恢复期间新语音明确提示等待。
- 关闭同时等待 voice 清理与 Backend.closed；后者必须证明自有 CLI/播放器真正退出。
  当前未装配真实 Backend，不能把此合同的单测当作 mpv 回收证明。

## 官方接入 gate

[网易官方说明](https://github.com/NetEase/skills)要求开发者 `appId/privateKey`、配置与用户登录。
普通/会员账号不能替代开发者授权；请在本地完成，不把私钥发到聊天、日志或 Git。

批准依赖：Node.js 24 LTS、`@music163/ncm-cli@0.1.7`、唯一 mpv 后端；实际版本、CLI
`commands`/命令帮助仍需取证。不得杜撰 `--json`、终端正则、私有 API 或兼容解析。

以下全部证明后才编写唯一异步 QProcess 适配器（argv，不拼 Shell）：

1. 机器可读的搜索、错误、state 字段与失败合同；`visible=false`、账号/区域限制明确不可播。
2. 当前歌曲 ID、Playing/Paused/EOF 和音量能读回；退出码不是效果证明。
3. 专属配置/IPC/PID 的 mpv，不控制用户已有播放器，不受共享配置/内部队列干扰。
4. 自然 EOF 不自动下一首；关闭和脚本停止后自有进程消失，其他播放器不受影响。
5. 官方认证续期可保留，但须核实是否重放业务及次数；无法限定时另行确认。
   控制层不自动登录、不重发音乐变更、不回退。

任一项不能证明即停在阶段③并报告，不自动改网页嵌入、非官方 API、其他播放器或音乐平台。

## 复现与验证

WSL Ubuntu-24.04，仓库 `/home/projects/CabinFlow`：

```bash
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCABINFLOW_INFERENCE_DIR=/home/projects/CabinFlow/runtime/build/inference
cmake --build runtime/build/voice-demo -j4
ctest --test-dir runtime/build/voice-demo --output-on-failure
python3 -m unittest discover -s agent/tests -p 'test_*.py' -v
```

运行仍用 `bash agent/scripts/voice_demo.sh start` / `stop`。当前无音乐子进程；未来须补真实
QProcess 与脚本 SIGTERM 清理验证，不称已完成。截图命令（新目录，不加载模型）：

```bash
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  runtime/build/voice-demo/frontend/voice_client_test \
  --preview runtime/build/music-cockpit-preview-new
```

输出 1440×900、1080×740、720×580 和窄窗口滚动到语音面板的 PNG；音乐未启用、车辆未知。
最终界面截图在 `runtime/build/music-cockpit-preview-final-20261004/`。
初次截图在 `runtime/build/music-cockpit-preview-20261004/`，该目录还保留首轮旧意图断言
失败的 `initial-full-ctest.log`，不覆盖首次失败。

首轮受影响测试 4/4；全套首次 34/35，旧测试将“播放音乐”视为未知。更新音乐合同并以
“打开天窗”保留未知意图断言后，全套 35/35；独立 Python 27/27。
Sol max 后审报告取消重入 P1、失败搜索复用编号 P2、EOF 测试缺失 P2；均已修正并补回归，
修正后的受影响三项 Debug 3/3，随后最终全套 Debug 35/35，Pipeline/音乐控制各 repeat10 通过。
Sol max 限定复查确认三个发现关闭，无剩余 P1/P2；未把源码复查当作独立测试或真实 CLI 验证。

Luna low 的 ASan：Pipeline/TCP/音乐控制/实际 20 秒 deadline 四项通过，Qt test 仍因
LeakSanitizer 264 bytes/3 allocations 失败（栈涉及 Qt/PulseAudio）；未 suppression、未关闭 LSan、
未宣称第三方根因。修复后 Luna 重编译 Qt/音乐控制，三项 ASan（Qt/控制/deadline）复跑 3/3；
单次通过不证明间歇性泄漏已修复，首次失败保留。日志在 `runtime/build/music-validation-20261004/`，
最终结果为 `final-{asan-build,asan-ctest,debug-ctest,repeat}.log`。

控制层测试用类型化替身，不证明网易云联网、音频可听或真实播放器所有权。
待实际验收：两首可播歌曲、文本/WAV 搜索和选曲/暂停/切歌、真实降音恢复、联网/授权/版权错误、
自有播放器退出。已有 ASR、LLM、听验和 Qt/audio LSan 问题继续保留，不用本轮测试替代质量或硬件验收。
