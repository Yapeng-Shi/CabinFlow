# 第二周模型依赖盘点与 x86 文件锁定

2026-09-29，先盘点当前 WSL 工作树的旧模型文件，再按用户确认的路线锁定
x86 模型。`文件已获取并通过 SHA-256` 不代表能被当前构建加载，更不代表
真实模型已接入 Runtime。

| 能力 | 本地观察 | 状态 |
|---|---|---|
| ASR | `agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/` 内 int8 encoder/decoder/joiner、tokens、bpe 为被 Git 跟踪的实文件 | 可计算本地校验值；尚未在新链路加载或核对上游来源 |
| ASR float encoder | 同目录 `encoder-epoch-99-avg-1.onnx` 为 133 字节 Git LFS 指针 | 不可当作模型加载 |
| TTS | `agent/tts/models/single_speaker_fast.bin` 为 133 字节 Git LFS 指针，且被当前仓库忽略；旧 README 提供网盘说明，但未锁定可验证下载版本 | 模型字节缺失 |
| RAG embedding | `agent/automotive_edge_rag/models/onnx/model.onnx` 是 79 字节文本路径，指向本工作树中不存在的 blob；`model.safetensors` 也是 Git LFS 指针 | 模型字节缺失 |
| LLM | 本地未找到 `.gguf` 或 `.rkllm` 文件 | 尚未选型和获取 |

本地 ASR int8 资源 SHA-256（文件名相对于上述 ASR 模型目录）：

```text
db6f51551762e40e549166fe041ea3e45464370b595e9ad23f06478ec3794fbb  encoder-epoch-99-avg-1.int8.onnx
4b618d383af304cfae281dbf0a53e8bf442c2f0502256cd5694bd6567ebdd834  decoder-epoch-99-avg-1.int8.onnx
bdda356d6f9b8c2d7cee9ee0e26075fa537490f7fd06520be408d287073667b9  joiner-epoch-99-avg-1.int8.onnx
a8e0e4ec53810e433789b54a5c0134a7eaa2ffca595a6334d54c00da858841d3  tokens.txt
bcae393dbc5611be5ffa4c7ae0841558978a5a4f484008cb9dff3a2cc97ebe01  bpe.model
```

## 已锁定的 x86 模型文件

经用户确认保留仓库已有的 Sherpa ASR int8 文件，另选可复现的 x86 LLM/TTS。
ASR 文件已被公开仓库提交
`10ab1382def232a478a902929916578e39d78c4e` 跟踪，当前远端 `main`
也指向此提交；其 5 个文件的校验值见 `agent/models/asr-int8.sha256`。

| 组件 | 固定来源与修订 | 大文件 SHA-256 | 当前验证 |
|---|---|---|---|
| TTS | [sherpa-onnx 文档中的 MeloTTS zh/en](https://github.com/k2-fsa/sherpa/blob/master/docs/source/onnx/tts/pretrained_models/vits.rst)，文件取自 [模型仓库](https://huggingface.co/csukuangfj/vits-melo-tts-zh_en) `a0d5c6a264c0ef92d70d8661d8cc502d79627cd6` | `model.onnx`: `bf30582eb1b012250a35b1a4a80e7dfbcf8485e7bb9de0d95efbbeef0e4ad86d` | 19 个文件下载且逐个哈希一致，尚未加载推理 |
| LLM | [Qwen 官方 GGUF](https://huggingface.co/Qwen/Qwen3-0.6B-GGUF) `23749fefcc72300e3a2ad315e1317431b06b590a`，`Qwen3-0.6B-Q8_0.gguf` | `9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031` | 模型与随附文档下载且哈希一致，尚未加载推理 |

从仓库根目录运行：

```bash
bash agent/scripts/fetch_x86_models.sh
```

脚本只使用以上两个固定修订的 URL，无镜像回退或隐式重试。文件放在被 Git
忽略的 `runtime/build/models/`，并用 `agent/models/x86-models.sha256` 和
`agent/models/asr-int8.sha256` 逐文件校验。已在当前 WSL 环境实际运行，
27 个文件校验通过。TTS 模型仓库随附 MIT `LICENSE`，Qwen 仓库随附
Apache-2.0 `LICENSE`；这里只记录上游文件，不替代使用场景的许可证审查。

曾检查旧 TTS 的 GitHub 发布压缩包，实际压缩内容缺少模型仓库中的
`tokens.txt`、`lexicon.txt` 等文件，所以没有把它当作本项目的获取入口。
仍需为 Sherpa/llama.cpp 推理库固定版本并做加载烟测；真实 ASR、LLM 和
TTS 替换属于计划第 3 步。不能用旧指针文件的 `oid` 冒充已获取模型。

## 2026-09-30：推理库锁定与独立加载证据

以上初始盘点是 2026-09-29 的快照，不是当前缺文件结论。
当前模型、库构建、加载和业务验收分别登记如下；详细执行命令和失败证据见
[独立烟测记录](model-smoke-results-20260930.md)。

| 组件 | 文件已校验 | 库构建/获取 | 真实加载/推理 | 接入 Runtime | 真实全链验收 |
|---|---|---|---|---|---|
| ASR int8 | 是 | Sherpa 1.11.3 CPU shared 上游包固定哈希；不是本地源码构建 | 2 次加载并解码；参考转写未评估 | 否 | 否 |
| Qwen3 Q8_0 | 是 | llama.cpp b6500 固定提交、本地 Release CPU 构建成功 | 已加载并生成；两配置下 `1+1 → 2` 验收均失败 | 否 | 否 |
| MeloTTS ONNX | 是 | 同一 Sherpa 1.11.3 包，实际 ORT 1.17.1 | 2 次生成可解码非静音 WAV；人工听验未完成 | 否 | 否 |

固定依赖归档与实际 Sherpa 产物的哈希分别位于
`agent/models/x86-inference.sha256`、`sherpa-runtime.sha256`。准备入口是
`bash agent/scripts/prepare_x86_inference.sh`；模型命令在明确输出范围下使用
`agent/scripts/run_model_smoke.py`，无下载、模型切换或失败重试。
固定输入与采样配置登记于 `agent/models/model-smoke-inputs.json`；整体 smoke
仍 failed，不把独立 TTS 成功或子进程退出 0 标成第二周第 2 步验收通过。
