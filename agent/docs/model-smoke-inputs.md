# 独立模型烟测输入合同

登记日期：2026-09-30，先登记输入，再运行真实推理。机器可读合同位于
`agent/models/model-smoke-inputs.json`。仅验模型独立加载，不接入 Runtime，
不修改 fake 链，不冒充第二周真实语音端到端验收。

| 组件 | 固定输入 | 预先定义的烟测判据 | 不证明什么 |
|---|---|---|---|
| ASR | 仓库已有 `test_wavs/0.wav`；16 kHz、mono PCM16、160850 帧、10.053125 秒；SHA-256 见 JSON | int8 encoder/decoder/joiner 加载成功；完整解码输出非空文本 | 尚无参考转写、WER/CER 或原录音许可确认，不作为正式语义验收集 |
| LLM | `1+1等于多少`；明确 ChatML/no-think 输入；seed=42、16 token 上限、512 context；采样参数见修订记录 | 加载已锁定 Q8_0 GGUF，输出数字 `2`；排除固定 CLI 自身的终止提示 | 不证明座舱问答、RAG、流式首 token 或车控能力 |
| TTS | `你好，这是离线智能座舱语音测试。`；sid=0、speed=1 | 模型加载成功，输出可解码且非静音的 mono PCM16 WAV | 可懂度需人工听验；不能用文件非空推断可听语义正确 |

全部使用 x86 CPU、2 threads；计时边界为一次独立子进程启动至结束（包含加载），
不冒充稳态推理、全链时延或声卡播放时延。输出、命令和环境保存在指定输出目录，
不复用旧结果作为本次成功。

推理依赖：Sherpa 1.11.3 CPU shared 的上游发布包（不是本项目源码编译）；
llama.cpp b6500 / `a7a98e0fffed794396b3fbad4dcdbbc184963645` 本地 Release 构建。
获取固定 URL、哈希和构建选项见 `prepare_x86_inference.sh` 与
`agent/models/x86-inference.sha256`。后续 Runtime 模型节点装配必须先关闭相关待决门槛。

## 输入修订记录

- revision 1：temperature=0，首次完整烟测输出 `1` 而非 `2`；失败报告保留在
  `runtime/build/inference/smoke-20260930-luna-1/report.json`。verbose token 日志确认
  输入的两个 `1` 与 `+` 正确；关闭 Flash Attention 仍返回 `1`，不能据此认定根因。
- revision 2：问题与期望 `2` 不变，按已锁定 Qwen 模型卡 Best Practices 的
  non-thinking 推荐值登记 temperature=0.7、top_p=0.8、top_k=20、min_p=0、
  presence_penalty=1.5；这是显式的新配置验证，不是运行失败后自动重试。
  不据此宣称采样配置是 revision 1 失败的确定根因。
- 固定 b6500 的 `tools/main/main.cpp` 明确把 ` [end of text]` 写入 stdout。
  烟测只排除这一已知 CLI 提示，模型答案仍须为 `2`；无结束提示视为未完成。
