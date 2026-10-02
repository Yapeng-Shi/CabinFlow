# 2026-09-30 独立模型加载与推理烟测

执行范围：实施计划第 14 节，第二周第 2 步。没有新增 Runtime 接口、模型节点、
RAG 或车控实现；D1、D5～D9 等待决合同没有被默认批准。整体烟测仍未通过：
LLM 已加载并生成文本，但两次预登记配置均未满足 `1+1 → 2`。

## 环境与依赖证据

- WSL2 Ubuntu 24.04.1 LTS，x86_64；kernel `6.6.87.2-microsoft-standard-WSL2`；
  Intel Core i7-7700K，WSL 可见 8 logical CPUs。
- Python 3.12.3，CMake 3.28.3；本地构建使用 `/usr/bin/cc` 11.5.0、
  `/usr/bin/c++` 13.3.0；Release、CPU，模型每进程 2 threads，单进程顺序执行。
- Sherpa [v1.11.3](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.11.3)，
  上游提交 `31ced58f9a5b6899ab8598bf3f55333982e4f4a4`；采用
  `sherpa-onnx-v1.11.3-linux-x64-shared.tar.bz2`，不是本项目源码构建。
  发布构建选择 Release/shared/TTS，源配置见该提交 `.github/workflows/linux.yaml`。
  随附 ONNX Runtime 经 `OrtGetApiBase()->GetVersionString()` 实际查询为 1.17.1。
  归档及两个 CLI、三个动态库逐项 SHA-256 固定并校验。
- llama.cpp [b6500](https://github.com/ggml-org/llama.cpp/releases/tag/b6500)，
  提交 `a7a98e0fffed794396b3fbad4dcdbbc184963645`。固定源码归档 SHA-256，
  本地构建 `llama-cli` 成功；运行 `--version` 确认为 6500/上述提交。
  GGML 随该提交锁定，不使用 system GGML；CURL、GPU、BLAS、OpenMP 关闭，
  具体配置见准备脚本和 `runtime/build/inference/llama-build/CMakeCache.txt`。
  本地产物 SHA 记录在 `runtime/build/inference/llama-cli.sha256`；它用于检测
  准备后的产物变化，不是上游签名或跨环境通用构建哈希。

模型来源和原始校验值继续由 [依赖审计](model-dependency-audit.md)管理；
固定输入、采样配置与修订理由见 [输入合同](model-smoke-inputs.md)。

## 实际结果

| 项目 | 结果 | 证据边界 |
|---|---|---|
| 依赖准备 | 通过 | 固定归档/发布库校验与本地 llama-cli 构建；不宣称 Sherpa 已本地源码构建 |
| ASR | 2 次加载/解码烟测通过 | 使用仓库已有 int8 三模型和固定音频；转写为 `昨天是 MONDAYS TOMORROW是星`，尚未与参考转写比对 |
| LLM revision 1 | 失败 | 子进程退出 0、模型加载并生成 `1`，期望为 `2`；进程成功不等于答案正确 |
| LLM revision 2 | 失败 | 显式采用已锁定 Qwen 模型卡的 non-thinking 采样配置，问题与期望不变，仍为 `1` |
| 独立 TTS | 2 次结构/非静音烟测通过 | 44.1 kHz mono PCM16；123027 / 123212 帧；人工听验仍未完成 |
| runner 单元测试 | 13/13 通过 | 缺文件、LFS 指针、坏哈希、静音/截断 WAV、失败/超时、坏配置报告、旧目录拒绝、CLI 提示处理 |
| Runtime Debug 回归 | 27/27 CTest 通过 | 执行现有标准入口；本轮未改 Runtime 源码或库依赖 |
| ASan/TSan | 本轮未执行 | 不把此前记录或 Debug 成功当作这次 sanitizer 结果 |

完整运行的输出目录分别为：

- `runtime/build/inference/smoke-20260930-luna-1/`：revision 1，总体 failed；
- `runtime/build/inference/smoke-20260930-revision2-1/`：revision 2，总体 failed；
- `runtime/build/inference/smoke-20260930-tts-luna-1/`、`smoke-20260930-tts-luna-2/`：
  仅 `component=tts` passed，不能替代整体成功。

每个目录保留 `report.json`、逐组件 stdout/stderr；成功 TTS 目录另含本次 `tts.wav`。
输出目录被 Git 忽略，不作为仓库内模型/音频交付物；源码、固定哈希和本记录可追溯。
两次真实 TTS WAV 字节不同符合当前语义合同，不能要求与 fake fixture 一样逐字节重复。

LLM 定位已执行：verbose token 日志确认两个 `1` 与 `+` 正确；关闭 Flash Attention
仍返回 `1`。本次日志中两个 `1` 与 `+` 的 token 正确，关闭该开关后答案未改变，
仍不足以确认或排除完整根因；
没有换模型、回退 fake 或把期望改为 `1`。b6500 的终止提示只是 CLI 输出，
剥离后错误答案仍为 `1`，有回归测试保护。

## 复现命令

以下准备、单测、Debug 与模型命令均已实际执行；再次烟测需指定新的输出目录。

```bash
cd /home/projects/CabinFlow
bash agent/scripts/prepare_x86_inference.sh
python3 -m unittest discover -s agent/tests -p test_model_smoke.py -v
python3 agent/scripts/run_model_smoke.py --output-dir runtime/build/inference/smoke-20260930-revision2-1
python3 agent/scripts/run_model_smoke.py --component tts --output-dir runtime/build/inference/smoke-20260930-tts-luna-1
cd runtime
./scripts/test.sh
```

第一条模型命令当前预期退出 1：它真实暴露未通过的 LLM 验收，不是假装可用的一键闭环。
新目录是强制要求，旧目录已存在时显式拒绝，不覆盖既有失败证据。
烟测计时为独立子进程启动到退出、包含加载；本轮没有稳态/P50/P95、首 token、
声卡播放或全链时延报告。

## 审查与下一步

GPT-6 Luna 按指定清单执行测试；GPT-6 Astra 只读审查入口/测试及实际报告。
审查发现并修复了坏配置留下 running 报告的问题；增量复审未发现回退或验收放宽。
该审查不覆盖 Runtime 既有待决生命周期风险，也不证明人工可听或端到端语义正确。

仍需定位并关闭 LLM 答案验收失败，确认 ASR 正式参考转写/录音许可与 TTS 人工听验。
三个组件均未接入 Runtime；不能越过生命周期、非法 final、外部准入等专项门槛。
本次未提交或推送，未删除任何历史迁移输入。
