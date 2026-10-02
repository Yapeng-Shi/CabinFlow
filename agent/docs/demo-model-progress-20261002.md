# 2026-10-02 Demo 模型定位记录

当前执行 [Demo 计划](../../plans/IMPLEMENTATION_PLAN.md)步骤 2。模型组件验收尚未通过，
没有接入真实 Runtime 流水线，没有完成 Qt、播放或 FakeVehicle 演示。
旧的 [2026-09-30 失败记录](model-smoke-results-20260930.md)保留，不覆盖或改写。

## 已确认边界

- 用户允许使用生成的中文音频作为固定输入；必须标记 synthetic，不宣称真人录音评测。
- 用户要求继续定位 Qwen3-0.6B、保持非思考模式；暂不替换为 1.7B。
- 原验收问题与期望不变：`只输出一个数字：1+1等于多少？ /no_think` → `2`。
  不通过换题、改参考、挑随机种子或采用思考模式把这条失败标为通过。
- 临时标量构建和 Q8 解码得到的 F32 副本只用于诊断；未接入生产，没有新模型回退路径。
- 用户随后批准同型号原始权重 + Transformers/CPU PyTorch 的独立诊断；也仅用于对照，不切换 Demo。

## LLM：复现与对照

环境：WSL2 Ubuntu 24.04 / x86_64，Intel Core i7-7700K；模型计算使用 2 threads，
单进程顺序执行。沿用 llama.cpp b6500，提交
`a7a98e0fffed794396b3fbad4dcdbbc184963645`；C 11.5.0 / C++ 13.3.0。
以下 GGUF 探针是此环境下的有限对照；后续独立引擎结果见下节，均不是 RK3576 验证。

实测原模型 SHA-256 仍为
`9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031`；
原 `llama-cli` 仍为
`aa65a92fbcc870af529c0d57b5ab0a320fb6ed58a64f142bb7d8d327d95b5b7d`，
分别与登记 manifest 一致。没有替换生产模型、CLI、采样配置或验收 parser。

基线 `smoke-demo-20261002-baseline-1/report.json`：LLM 子进程退出 0，
实际输出 `1 [end of text]`，剥离 CLI 终止提示后仍为 `1`；runner 语义验收退出 1。
进程成功不等于回答正确。

| 对照 | 实际证据 | 能说明什么 |
|---|---|---|
| 原输入、分词与采样参数 | 原 prompt 共 27 tokens，两个 `1` 与 `+` 正确；参数符合登记 | 未发现实际输入/参数偏离 |
| 上游原生 Jinja，`enable_thinking=false` | 原生模板与登记 prompt 逐字节相同 | 当前手工拼接与本模型模板没有差异 |
| 直接读取采样前 logits | 全部有限；`1` 为 27.5277，`2` 为 20.8414 | 错答倾向在采样前已经存在，不是输出 parser 把 `2` 改成 `1` |
| 原 API 逐 token 解码 / 批量解码 | 上述打印分数一致 | 本次探针未发现两种解码方式导致差异 |
| 同提交关闭 `GGML_LLAMAFILE` | 上述打印分数一致 | 关闭该优化没有修正此次倾向 |
| 同提交关闭 CPU SIMD 构建 | `1` 为 27.4834，`2` 为 20.9295；第一候选仍为 `1` | 数值有变化，但没有反转该候选排序 |
| 同一 Q8 权重解码为 F32 | `1` 为 27.6411，`2` 为 21.0127；第一候选仍为 `1` | 浮点计算对照也没有反转该候选排序；不等于原始 BF16 权重对照 |
| 官方原始权重 + Transformers CPU | `1` 为 27.826839，`2` 为 21.031212；诊断回答 `1` | 同一输入在独立引擎下也复现错答，不是 GGUF/llama.cpp 路径独有现象 |

logit 是模型给候选 token 的未归一化分数，不是准确率或概率。
表中的探针只检查第一个回答 token，不把它当作完整答案验收通过。
前述 GGUF 探针仍使用同一版 llama.cpp；这些探针本身不能排除该引擎共用实现的问题。
Q8→F32 不会恢复量化前已损失的信息，因此不能证明原始模型也必然错答。

其他已执行对照：扩大 context、英文同义问题、简化中文问题、移除软 `/no_think`
指令、增加普通 system 消息，没有关闭原失败。允许思考的单独诊断最终答 `2`，
但用户没有批准采用该模式，因此不进入 Demo 配置，也不作为非思考验收成功。

`-b 1 -ub 1` 的 CLI 对照输出为空：b6500 `tools/main/main.cpp` 的循环末尾
检测 `embd.back()` 是否为 EOG，没有区分尚未消费完的 prompt；该配置分块读取时
会在请求里的 `<|im_end|>` 处提前结束。这不是原基线的错答根因，
不能用这条空输出来证明模型计算崩坏；没有修改上游 CLI 或生产 batch 配置。

当前结论：独立引擎和原始权重也复现该非思考输入下的错答，支持将本例定位为
模型在该输入条件下的生成行为，而不是 CabinFlow 输出解析或 GGUF 接入独有的错误。
这不解释训练层面的原因，也不证明 GGUF 转换/引擎对所有输入均正确。
`1+1 → 2` 仍未通过；没有可交付的代码修复，不再把切换 CPU 开关当作已知修复方向。

### 独立原始权重对照

经用户批准，从官方 [原始权重目录](https://huggingface.co/Qwen/Qwen3-0.6B/tree/main)固定下载
提交 `c1899de289a04d12100db370d81485cdf75e47ca`；不是另一个型号。
`model.safetensors` 为 1503300328 bytes，SHA-256 为
`f47f71177f32bcd101b7573ec9171e6a57f4f4d31148d38e382306f42996874b`。
9 个源文件均校验来源 blob/LFS 指纹及实际 SHA-256；下载无自动重试。

隔离目录为 `runtime/build/inference/transformers-diagnostic-20261002/`。
WSL 最初缺少 ensurepip，因此安装 `python3.12-venv` 及其两个 wheel 包；0 个升级/删除。
诊断 venv 实际为 Torch 2.7.1+cpu、Transformers 4.51.3、safetensors 0.5.3、NumPy 2.2.6；
完整依赖见该目录 `python-packages.txt`。Torch 来自官方 CPU wheel index，
Transformers 安装使用机器已有的清华 PyPI 镜像配置，下载源见 pip 日志。
这些包没有加入项目生产依赖，原 CLI 和模型指纹仍一致。

`local_files_only=true`、`trust_remote_code=false`、网络离线开关，使用 CPU/FP32/eager attention，
2 个计算线程、1 个 inter-op 线程。原始 tokenizer 的非思考模板与登记 prompt 完全一致，
共 27 tokens，`1`/`2` 的 token ID 也一致。原始 BF16 权重转换为计算用 FP32；
不是从 Q8 解码出的临时 F32 文件。

先记录未采样 logits，再用 greedy 生成最多 16 个 token 作诊断，结果 `1`，与期望 `2` 不同。
greedy **不是**正式烟测所登记的采样器，不能将诊断进程退出 0 当作正式语义验收通过。
此对照的主要依据是采样前分数和跨引擎复现，不是换种子挑答案。
报告明确 `original_question_matches=false`、`registered_smoke_passed=false`。

已实际执行：

```bash
cd /home/projects/CabinFlow/runtime/build/inference
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
  transformers-diagnostic-20261002/venv/bin/python diagnose-original-qwen-20261002.py
```

stdout/stderr 保存在隔离目录，结果为 `original-reference-1/report.json`。
后续若调整通用提示词或生产配置，必须明确登记新合同并保留原失败；当前未批准/实施这种调整。

## 生成语音：已有结果与缺口

固定文本、参考与未来 Demo 预期见 [demo-speech-inputs.json](../models/demo-speech-inputs.json)。
使用已锁定 MeloTTS 与许可文件，5 段均成功生成非静音 mono PCM16 / 44.1 kHz WAV。
随后同一 Sherpa 1.11.3 CPU/int8 ASR 对这 5 段各解码一次。
比较只消除 Unicode 标点和空白，不改词语、数字或参考：

| 用例 | 参考 | ASR 实际文本 | 精确匹配 |
|---|---|---|---|
| greeting | 你好请介绍一下你自己 | 你介绍 | 否 |
| arithmetic | 一加一等于多少 | 一加一等于多少 | 是 |
| capital | 法国的首都是什么 | 法国 | 否 |
| ac_on | 请打开空调 | 你打 | 否 |
| ac_off | 请关闭空调 | 醒关闭声茬 | 否 |

本次只有 1/5 与登记参考匹配，脚本整体退出 1。
5 段的帧数、时长、SHA-256 和逐阶段命令保存在 `demo-speech-20261002-1/report.json`。
人工听验仍未完成，尚不能判断错误来自合成可懂度、ASR 还是输入处理。
这不是最终 5×3 次 Demo 验收，不声称语音准确率或真实回答已可用。

## 执行与证据路径

以下命令已实际执行；模型脚本要求全新输出目录，不能覆盖旧证据：

```bash
cd /home/projects/CabinFlow
python3 -m unittest discover -s agent/tests -p 'test*.py' -v
python3 agent/scripts/run_model_smoke.py --output-dir runtime/build/inference/smoke-demo-20261002-baseline-1
python3 agent/scripts/generate_demo_speech.py --output-dir runtime/build/inference/demo-speech-20261002-1
```

单元测试 18/18，退出 0；上述两条真实模型命令均退出 1。没有用绿的 runner 单测
代替模型语义验收。本轮未改 Runtime，没有新增 Runtime 或 sanitizer 验证结果。

诊断源、构建缓存、转换副本与日志在 Git 忽略的 `runtime/build/inference/` 下：

- `diagnostic-demo-20261002-1/`：基线/分词、原始及各对照 logits、构建/转换日志；
- `diagnostic-demo-prompts-20261002-1/`、`diagnostic-demo-mode-20261002-1/`：预登记问题/模式对照；
- `diagnose-demo-logits-20261002.cpp`：临时原生模板、直接 API 与逐 token 探针；
- `diagnostic-demo-20261002-1/diagnostic-artifacts.sha256`：当前探针源、F32 副本与原 CLI 指纹。
- `transformers-diagnostic-20261002/`：原始权重、下载校验、隔离依赖、独立参考结果；
  `download-original-qwen-20261002.py` / `diagnose-original-qwen-20261002.py` 为本次临时脚本。

这些临时构建材料不是 Git 交付物；全新克隆可用已入库的 smoke runner 复现原失败，
不能声称克隆后已经具备上述所有探针。原始证据保留，无静默重试、删除或覆写。

GPT-6 Luna 执行 18 项单测与只读证据核验。此前 GPT-6 Astra 已审查部分输入、
采样与计算路径定位；后续审查遇到额度限制，新增标量/F32/独立参考没有完成 Astra 复审。
本记录不宣称审查通过或模型里程碑完成。

## 后续执行主线：系统集成，不继续模型定位

用户随后明确要求先搭系统、组件解耦并方便替换。上述诊断与质量失败保留原始范围，
不再阻塞集成。已新增 ASR/LLM/TTS 的 Agent 端口与具体适配器、可复用编排和真实 CLI；
文本与 WAV 两个入口已实际贯通到真实 TTS 文件，没有更换 0.6B 非思考模型。
文本单例得到北京；WAV 单例仍只转写出“法国”，质量问题未解决。Qt/TCP 全链、播放、
FakeVehicle 与人工听验尚未完成。新的实现审查和测试不倒推上述历史诊断已经完成审查。
最新构建/运行/测试结果见 [真实组件集成记录](voice-demo-integration.md)。
