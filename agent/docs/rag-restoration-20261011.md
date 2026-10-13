# 原 RAG 业务恢复（2026-10-11）

## 实现范围

用户批准在当前 gRPC/Qt/CAN Demo 恢复旧 Agent 业务。第一步是 RAG 与原分级路由，后续录音/流式阶段尚未实施。没有改 Runtime 核心、Gateway、CAN 后端、LLM 模型，也没有重新接入旧 Infra。

```text
文本 / WAV → 原 Router 分级 → rag.primary（RagBackend）
  RAG_ONLY：命中 → 标注来源的资料 → TTS；正常无命中 → 未找到相关资料 → TTS
  LLM_ONLY：原问题 → LLM → TTS
  HYBRID：命中 → 原问题 + <rag> + 资料 → LLM → TTS
          正常无命中 → 原问题 → LLM → TTS
  检索失败 / 取消 / 非法结果 → 最终失败或取消，不进入 LLM/TTS
```

音乐与明确车控规则优先，RAG/LLM 文案不能执行 CAN 动作。`AnswerRoute`、`RagResult` 属于 AgentProtocol；Runtime/Gateway 只处理通用 Message。原 emergency/factual/creative/complex/unknown 的顺序、词表、重复词计数、UTF-8 字节计分与路由映射保持，测试编译旧 classifier 仅作 characterization，不接入生产。

### Python 所有权

唯一生产实现是 `PythonRagBackend`，经私有 pybind11 调用原 Transformers/BERT attention-mask mean pooling、128-token 截断和 top-1 cosine >= 0.5 算法。安全读取旧 JSON/NPY，`allow_pickle=False`；不读 pickle、不重建索引、不下载模型、不使用查询缓存或第二后端。

应用装配线程创建解释器/模型并释放 GIL；`rag.primary` 的单 worker 在调用期间取得 GIL、返回拥有数据的 C++ 结果。取消不能硬中断 Torch：检查推理前后，取消后丢弃结果，先等 handler/drain，再停止 Runtime，最后在装配线程重新取得 GIL、释放对象、终结解释器。每进程只允许一次解释器生命周期；不支持重启。Python 不安装信号处理器，服务退出仍由现有 C++ `sigwait` 控制。

### 数据与失败证据

沿用 `agent/automotive_edge_rag/python/vector_db` 的 45×768 float32 索引。它是历史示例资料，包含虚构车辆参数、服务号码，且第 18 行标题“如何开启车道保持”与发动机故障正文错配；不作为真实车辆操作或安全建议。只恢复检索/路由机制，资料真实性、许可来源和问答质量另行验收。

索引输入 hash：

- JSON：`59303bca7ab57724e8d4233a738d29b1105d5ccd0db8782ab69ad4e2626acfb3`
- NPY：`127f6cb1be4079e7550f6e16c37fd06db6918089d8f6ab11c0d04ae7d03acf94`

原 Git LFS 模型指针已按相同内容恢复真实 `shibing624/text2vec-base-chinese` 权重，官方 revision `183bb99aa7af74355fb58d16edf8c13ae7c5433e`；safetensors SHA-256 `0c855515479137398ce4ea985628548d4e8ed8c5764656dac966d6a24f39e721`，完整资源清单在 `agent/models/rag-model.sha256`。

此前 llama.cpp BERT 候选失败：长 ASCII 词/组合音标分词不等价、多项向量误差超过固定 `1e-4`，127-token 截断向量曾未被实际观察。最终失败记录保留在 `runtime/build/inference/rag-parity-20261011-{a,b,c}/`；撤下候选源码/构建目标/GGUF 准备路径，不保留运行时回退。旧 GGUF/工具仅是 Git 忽略的诊断产物，不由生产读取。

## 依赖与重现

当前 WSL x86/Ubuntu 24.04：CPython 3.12.3、pybind11 2.11.1、Torch 2.7.1+cpu、Transformers 4.51.3、NumPy 2.2.6；RAG CPU 2 threads。主依赖版本在启动时校验，全部传递依赖的实际 freeze 记录保存在本轮执行证据目录。

构建开发包（本机已安装；下面合并安装命令未另重跑）：

```bash
sudo apt-get install --no-install-recommends python3-dev=3.12.3-0ubuntu2.1 pybind11-dev=2.11.1-2 python3.12-venv
cd /home/projects/CabinFlow
bash agent/scripts/prepare_rag_model.sh
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCABINFLOW_INFERENCE_DIR=/home/projects/CabinFlow/runtime/build/inference
cmake --build runtime/build/voice-demo -j4
ctest --test-dir runtime/build/voice-demo --output-on-failure
```

准备脚本需要网络；实际查询全部 local-only。`voice_demo.sh` 已传入唯一固定 RAG 路径。直接调试 CLI/server 时，原 ASR/LLM/TTS 参数后必须追加：

```text
/home/projects/CabinFlow/runtime/build/inference/rag-python/venv/lib/python3.12/site-packages
/home/projects/CabinFlow/runtime/build/models/text2vec-base-chinese/hf
/home/projects/CabinFlow/agent/automotive_edge_rag/python/vector_db
```

正式检索对照命令（输出目录必须全新）：

```bash
runtime/build/inference/rag-python/venv/bin/python agent/scripts/check_rag_parity.py \
  --repo /home/projects/CabinFlow \
  --native-probe /home/projects/CabinFlow/runtime/build/voice-demo/rag_probe \
  --output-dir /home/projects/CabinFlow/runtime/build/inference/rag-python-parity-20261011
```

它测量 45 篇旧索引资料与 12 个中文/混合文本/特殊 token/长词/音标/截断边界输入，固定 `1e-4`，另用 3 个独立进程验证启动失败。每次 native probe 实际执行取消、非法输入、推理后取消、worker 查询及主线程析构；不是复用历史缓存。

## 当前证据

- Debug 编译通过；分级/流水线/gRPC 重点 3/3 通过，旧分级器 1036 条输入映射一致。RAG malformed-index Python 4/4 通过。
- 真实文本 `空调滤清器多久更换？` → RAG_ONLY → TTS：退出 0，命中文档 12，没有 LLM backend call，无车控动作。生成 44.1kHz mono PCM16 WAV，538112 帧、531191 非零样本。hash `519ab00fbb2ba70fa7d206c030051c16331dff6c5522e2a9f0d9e46f207cb8e5`。日志/音频/freeze 在 `runtime/build/inference/rag-pipeline-20261011-xGejA7/`。
- 最终 Debug 全套 39/39 通过；限定 ASan/LSan 的 `voice_pipeline_test`、`query_router_test` 2/2 通过（测试替身，不覆盖嵌入 Torch/Qt/gRPC sanitizer）。Sol `gpt-6.1-sol` max 只读设计/实现审查未发现可证明 P1/P2；这不是无缺陷证明。
- 真实服务加载所有模型、启动 gRPC、收到 SIGTERM 后正常退出 0；证据 `runtime/build/inference/rag-server-20261011-wuNhgM/`。测试自有 ECU 已退出，没有遗留服务；vcan0 按既有匹配内核准备重建，未换内核。
- 正式 57/57 条检索对照通过，固定 `1e-4` 未放宽，3/3 独立进程启动失败检查通过（missing-site/model/index，均退出 1）；每条 native probe 的 worker/取消/析构合同均运行。结果在 `runtime/build/inference/rag-python-parity-20261011/results.json`。由 `gpt-6-luna` low 执行的首轮 Debug 38/38（新增索引 CTest 注册前）、Bash 语法和诊断 runner Python 10/10 均通过，日志 `runtime/build/rag-python-cutover-20261011-ONFB36/`；随后主 agent 重建最终 Debug 39/39。
- 最终追加 Qt RAG 自动验收后，受影响 5/5 回归通过。真实 Wayland Qt → gRPC → RAG → TTS → 回答回传/WAV 归档 → PlayingState/停止/关闭通过；GUI 定时器在等待时持续运行，无车控动作。记录在 `runtime/build/inference/rag-qt-20261011-duXEI4/`，截图 `ui/rag-ui.png`，音频 `ui/answer.wav` hash `7e43c73c93fa90935bbf09aea11e18b6d60c6d71b37587b8ed2cd9bb0371634e`。自有 server/ECU 已正常退出；已有 Mesa/EGL 警告保留，不声称 GPU 加速或人工听验。没有用文本验收代替 WAV/ASR 质量验收。

当真实 server/ECU 已启动时，可复跑测试诊断入口（不是新增产品入口；必须换全新输出目录，PORT 使用真实 server 日志中的端口）：

```bash
QT_QPA_PLATFORM=wayland runtime/build/voice-demo/frontend/voice_client_test \
  --rag PORT /absolute/new/output-directory
```

仍未完成：Qt 人工操作/听验与正式 WAV 识别质量、真实资料校验、麦克风/流式 ASR、流式 LLM/句级 TTS/播放队列、RK3576/AAOS/真实 CAN。此前 gRPC TSan 和 Qt LSan 未关闭；本轮功能通过不能覆盖旧安全失败。
