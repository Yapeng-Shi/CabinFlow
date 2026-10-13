# 🚘 CabinFlow

### Offline voice interaction. A model-independent C++ foundation.

🌐 **English** · [简体中文](README.zh-CN.md)

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Linux](https://img.shields.io/badge/Linux-WSL2-333333?logo=linux)
![Protobuf](https://img.shields.io/badge/Protocol-Protobuf-4285F4)
![Local inference](https://img.shields.io/badge/Inference-Local_CPU-2E7D32)
![Qt](https://img.shields.io/badge/UI-Qt_6%20%2F%20QML-41CD52?logo=qt)
![Project stage](https://img.shields.io/badge/Stage-Cockpit_Simulation-orange)

CabinFlow is an **offline intelligent-cockpit voice Agent** built on a reusable
C++ Runtime. It brings local speech recognition, intent routing, language-model
inference and speech synthesis into a testable application pipeline.

The key idea: **separate the system from its models**. Communication, task
identity, queues, workers and cancellation belong to Runtime; algorithms belong
to Agent components. Models can evolve without rebuilding the entire application.

> 🚧 Active development: the Qt/Gateway boundary now uses gRPC on x86 CPU /
> WSL2 / Ubuntu 24.04. WSLg playback-start checks passed; human listening,
> model-quality acceptance and target-hardware validation remain open. Air-conditioning
> and left-front-window control now target a SocketCAN virtual ECU, never a real
> vehicle. A matching custom WSL kernel, real vcan request/ACK tests and the
> gRPC/Qt/local-model loop were verified on 2026-10-07. A 2.5D driver view visualizes
> actual simulated execution receipts. This is not physical CAN or vehicle validation.

> 🎵 The front-row panorama now includes a music HMI and typed voice commands.
> **NetEase playback is not enabled:** official developer authorization and CLI/player
> qualification remain pending. Voice inference stays local; online music is a separate
> network boundary. [Music milestone and limits](agent/docs/cockpit-music-20261004.md).

## 💡 Why CabinFlow?

A voice assistant needs more than model calls: it must connect stages, correlate
results with the right task, bound concurrent work, handle failures and shut down
safely. CabinFlow focuses on making that foundation explicit and reusable.

- **Offline by design:** inference uses local models, not cloud services. First-time dependency/model preparation requires network access.
- **Replaceable components:** ASR, RAG, LLM and TTS use small C++ interfaces with constructor injection; model SDKs stay inside adapters.
- **Local knowledge retrieval:** the original tiered router uses a local Transformers embedding model and historical example knowledge base; normal HYBRID misses reach LLM, while retrieval errors terminate the task. [Restoration and evidence](agent/docs/rag-restoration-20261011.md).
- **Runtime-managed execution:** each target has its own bounded queue and single worker, with admission, lifecycle and backpressure handled centrally.
- **Traceable tasks:** Protobuf messages carry trace, session, work and message identities, sequence, TTL and final markers.
- **Explicit failure and cancellation:** failures stop downstream processing; cancellation suppresses late results and waits for actual handler completion.
- **Visible cockpit simulation:** a 2.5D driver view shows climate airflow and left-front-window open/close transitions from typed execution receipts, not optimistic clicks or model claims. Later cancellation or speech failure does not undo an applied action.
- **Testable boundaries:** contract and integration tests cover protocols, task isolation, queues, lifecycle and component behavior.
- **Opt-in local wake input (acceptance pending):** the Qt client has a dedicated microphone/KWS/VAD worker and submits one complete WAV into the existing pipeline. The configured wake phrase has not passed synthetic-input detection or a human microphone trial yet. [Implementation and open gate](agent/docs/wake-input-20261013.md).

No automatic model fallback, legacy-format guessing or plugin framework is added
to the current pipeline.

## 🏗️ Architecture

```text
               Agent: application and algorithms

WAV ──→ ASR ──→ Dialogue / Router ──→ LLM ──→ TTS ──→ Answer WAV
Text ─────────→         │                     ↑
                        ├─ RAG_ONLY / HYBRID → Local RAG → direct TTS / LLM
                        └─ Climate / window intent → SocketCAN / vcan ECU → TTS
                           simulated state + execution receipt

                 Runtime: shared execution foundation
┌──────────────────────────────────────────────────────────────────┐
│ Message identity · Work lifecycle · Ledger · Bounded queues       │
│ Per-target workers · Cancellation · Transport · Observability     │
└──────────────────────────────────────────────────────────────────┘

Product entry: Qt/QML → gRPC Gateway → the application pipeline
CLI:           reuses the same execution chain for local integration checks
```

Runtime sees generic `Message` objects, not ASR/LLM/TTS SDK types. Agent nodes
decode domain payloads and use injected backends. Within a supported engine, a
model path can be changed; a new engine implements the corresponding backend
interface and is selected at startup.

The external Gateway uses **gRPC bidirectional streaming + RuntimeMessage Protobuf**,
with a 4 MiB message limit and an eight-message output queue including in-flight writes.
The former length-prefix framer is characterization input only, not a production fallback.
Internal transport supports InMemory and ZeroMQ; the current model Demo runs
in one process and uses Runtime-managed execution channels.

## 🛠️ Technology

| Layer | Stack |
|---|---|
| Systems | C++17, Linux, RAII, multithreading, bounded queues |
| Communication | gRPC, Protobuf, SocketCAN; internal InMemory/ZeroMQ; generic epoll/TCP library |
| Local inference | Sherpa-ONNX / ONNX Runtime, llama.cpp; Python/Transformers CPU RAG via pybind11 |
| Engineering | CMake, CTest, contract/integration tests, scoped ASan/TSan checks |
| Interface | Qt 6 / QML, asynchronous gRPC client, WAV playback via WSLg |

## 📍 Current scope

- **Implemented:** generic Runtime, typed gRPC control/data paths, injected inference adapters, reusable `VoicePipeline`, a one-screen Qt client, and a 2.5D cockpit with typed climate/window receipts; SocketCAN backend and a separate virtual ECU.
- **Verified on x86/WSL:** Debug 37/37, Python 28/28; real vcan ECU integration and scoped CAN ASan/TSan; Wayland Qt text/WAV, playback-start, vehicle receipts and cancellation. [Environment and evidence](agent/docs/wsl-vcan-20261007.md). Broader gRPC TSan findings remain open.
- **Historical evidence (before migration):** real text/WAV inputs through TCP and Runtime nodes; 15 fixed Qt trials archived answer WAVs/screenshots and observed playback-start/cancel-stop states.
- **Current extension:** panoramic QML cockpit and typed music control contracts; production music remains disabled pending official authorization and qualification. Hands-on listening, an operation video and the Qt/audio LeakSanitizer finding remain open. [Fixed CLI/Qt cases](agent/docs/voice-demo-acceptance-20261003.md) still fail speech-quality expectations.
- **Not claimed:** complete voice UX, overall model accuracy, production vehicle control, AAOS integration or physical RK3576 deployment.

The app supports one active task and non-streaming output. Qt provides cancellation
and waits for actual task cleanup before accepting another input; CLI writes audio
files and has no cancel control. Model-quality issues and human-listening gaps
remain recorded separately; execution and player state are not quality acceptance.

## 📂 Project layout

```text
runtime/                 Generic execution, network, Gateway and Transport
agent/
  apps/voice_demo/       Real-model CLI, gRPC server and Qt/QML frontend
  inference/             Backend interfaces and native SDK adapters
  pipeline/              Reusable voice orchestration
  dialogue/              Typed text node and rule-based routing
  vehicle/               SocketCAN backend / virtual ECU; FakeVehicle is test-only
  protocol/              Cockpit-domain Protobuf schemas
  scripts/               Dependency/model preparation and test runners
  tests/                 Agent contract and runner tests
plans/                   Implementation plan and module boundaries
docs/                    Repository-level migration records
```

Historical code remains migration input, not a production fallback. Capabilities
are tracked in the [migration ledger](runtime/docs/capability-migration.md).

See the [gRPC/CAN migration record](agent/docs/grpc-can-migration-20261006.md) and
[verified WSL setup](agent/docs/wsl-vcan-20261007.md). A fresh environment must
provide matching CAN_RAW/vcan support and an UP `vcan0` before starting the full Demo.

## 🚀 Getting started

Use Linux/WSL with a C++17 compiler, CMake/Make, Protobuf + `protoc`, pkg-config
and libzmq development files; gRPC requires `libgrpc++-dev` and `protobuf-compiler-grpc`. Preparation also needs curl, tar/bzip2 and SHA-256
tools. Run from the repository root; it is not itself a CMake entry point.

**Explore the Runtime without loading models:**

```bash
cd runtime
./scripts/test.sh
./build/linux-debug/apps/runtime_demo/runtime_demo
cd ..
```

This deterministic example exercises two sessions, ordering, expiry, final and
cancellation; it is not a speech demo. Use `runtime/scripts/`, not the historical
`runtime/build.sh` dependency installer.

**Prepare and build the voice application:**

Install the [Qt/audio dependencies](agent/docs/voice-demo-integration.md#qt-依赖与交互入口)
first; the standalone Runtime build does not require them.

```bash
bash agent/scripts/prepare_x86_inference.sh
bash agent/scripts/fetch_x86_models.sh
bash agent/scripts/prepare_rag_model.sh
bash agent/scripts/prepare_wake_models.sh
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR="$PWD/runtime/build/inference"
cmake --build runtime/build/voice-demo -j4
```

SDKs and downloaded LLM/TTS files are Git-ignored local artifacts, not included
in a fresh clone. Scripts pin sources and checksums. Runtime includes ZeroMQ
build targets even when the Demo uses in-process execution.

**Start or stop the cockpit Demo from WSL:**

```bash
bash agent/scripts/voice_demo.sh start
bash agent/scripts/voice_demo.sh stop
```

The launcher requires an already configured, UP `vcan0`, starts its own virtual ECU,
then waits for the backend before opening the WSLg window. It never changes the WSL kernel. Closing the
window does not stop the backend; use `stop` to shut down its recorded processes.
The virtual interface is temporary: recreate it after a WSL restart using the setup
commands in the environment record. No interface auto-recovery is installed.
Logs are kept under `runtime/build/voice-demo/launcher/run-*/`. It does not
automatically build, download models or force-kill a server still cleaning up.

See the [voice application guide](agent/docs/voice-demo-integration.md) for the
backend/frontend launch commands, model paths, CLI results and verification. Inputs
must be complete **16 kHz mono PCM16 WAV** files; the full encoded message body
must fit within **4 MiB**, and each output directory must be new.

## 🧭 Roadmap

1. **Experience acceptance:** hands-on listening, fixed voice cases and cancellation demos.
2. **Showcase:** reproducible commands, scoped measurements and an operation video.

Original tiered RAG is wired into Agent. The single-utterance wake-input code is
integrated, but wake-phrase detection and real microphone acceptance remain open.
Streaming stages, multi-occupant interaction, AAOS and hardware optimization are deferred.

## 📚 Learn more

- [Implementation plan](plans/IMPLEMENTATION_PLAN.md) · [Directory plan](plans/RUNTIME_DIRECTORY_STRUCTURE.md)
- [Runtime architecture](runtime/docs/architecture.md) · [Build and development](runtime/docs/development.md)
- [Real-component integration and test evidence](agent/docs/voice-demo-integration.md)
- [Fixed-case results, timing boundaries and showcase checklist](agent/docs/voice-demo-acceptance-20261003.md)
- [Model versions and dependency audit](agent/docs/model-dependency-audit.md) · [Quality and known failures](agent/docs/demo-model-progress-20261002.md)
- [Runtime migration progress](runtime/docs/runtime-migration-closeout.md) · [Capability ledger](runtime/docs/capability-migration.md)
- [Fake voice demo](agent/docs/fake-voice-milestone.md): message/state validation with test audio, not real inference or a fallback.
