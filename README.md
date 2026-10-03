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

> 🚧 Active development: real models now connect to a TCP/Qt interface on x86 CPU /
> WSL2 / Ubuntu 24.04. WSLg playback-start checks passed; human listening,
> model-quality acceptance and target-hardware validation remain open. Air-conditioning
> and left-front-window control are explicitly simulated by FakeVehicle, never sent
> to a real vehicle. A 2.5D driver view visualizes validated execution receipts.

## 💡 Why CabinFlow?

A voice assistant needs more than model calls: it must connect stages, correlate
results with the right task, bound concurrent work, handle failures and shut down
safely. CabinFlow focuses on making that foundation explicit and reusable.

- **Offline by design:** inference uses local models, not cloud services. First-time dependency/model preparation requires network access.
- **Replaceable components:** ASR, LLM and TTS use small C++ interfaces with constructor injection; model SDKs stay inside adapters.
- **Runtime-managed execution:** each target has its own bounded queue and single worker, with admission, lifecycle and backpressure handled centrally.
- **Traceable tasks:** Protobuf messages carry trace, session, work and message identities, sequence, TTL and final markers.
- **Explicit failure and cancellation:** failures stop downstream processing; cancellation suppresses late results and waits for actual handler completion.
- **Visible cockpit simulation:** a 2.5D driver view shows climate airflow and left-front-window open/close transitions from typed execution receipts, not optimistic clicks or model claims. Later cancellation or speech failure does not undo an applied action.
- **Testable boundaries:** contract and integration tests cover protocols, task isolation, queues, lifecycle and component behavior.

No automatic model fallback, legacy-format guessing or plugin framework is added
to the current pipeline.

## 🏗️ Architecture

```text
               Agent: application and algorithms

WAV ──→ ASR ──→ Dialogue / Router ──→ LLM ──→ TTS ──→ Answer WAV
Text ─────────→         │                     ↑
                        └─ Climate / window intent → FakeVehicle → TTS
                           simulated state + execution receipt

                 Runtime: shared execution foundation
┌──────────────────────────────────────────────────────────────────┐
│ Message identity · Work lifecycle · Ledger · Bounded queues       │
│ Per-target workers · Cancellation · Transport · Observability     │
└──────────────────────────────────────────────────────────────────┘

Product entry: Qt/QML → TCP Gateway → the application pipeline
CLI:           reuses the same execution chain for local integration checks
```

Runtime sees generic `Message` objects, not ASR/LLM/TTS SDK types. Agent nodes
decode domain payloads and use injected backends. Within a supported engine, a
model path can be changed; a new engine implements the corresponding backend
interface and is selected at startup.

The existing external Gateway uses **4-byte big-endian length + Protobuf**.
Internal transport supports InMemory and ZeroMQ; the current model Demo runs
in one process and uses Runtime-managed execution channels.

## 🛠️ Technology

| Layer | Stack |
|---|---|
| Systems | C++17, Linux, RAII, multithreading, bounded queues |
| Communication | TCP, epoll Reactor, Protobuf, ZeroMQ |
| Local inference | Sherpa-ONNX / ONNX Runtime, llama.cpp |
| Engineering | CMake, CTest, contract/integration tests, scoped ASan/TSan checks |
| Interface | Qt 6 / QML, asynchronous TCP client, WAV playback via WSLg |

## 📍 Current scope

- **Implemented:** generic Runtime, typed TCP control/data paths, injected inference adapters, reusable `VoicePipeline`, a one-screen Qt client, and a 2.5D cockpit with FakeVehicle climate and left-front-window receipts.
- **Executed:** real text/WAV inputs through TCP and Runtime nodes; 15 fixed Qt trials archived answer WAVs/screenshots and observed playback-start/cancel-stop states.
- **Next:** hands-on listening, an operation video and diagnosis of the current Qt/audio LeakSanitizer finding. [Fixed CLI/Qt cases](agent/docs/voice-demo-acceptance-20261003.md) still fail speech-quality expectations.
- **Not claimed:** complete voice UX, overall model accuracy, production vehicle control, AAOS integration or physical RK3576 deployment.

The app supports one active task and non-streaming output. Qt provides cancellation
and waits for actual task cleanup before accepting another input; CLI writes audio
files and has no cancel control. Model-quality issues and human-listening gaps
remain recorded separately; execution and player state are not quality acceptance.

## 📂 Project layout

```text
runtime/                 Generic execution, network, Gateway and Transport
agent/
  apps/voice_demo/       Real-model CLI, TCP server and Qt/QML frontend
  inference/             Backend interfaces and native SDK adapters
  pipeline/              Reusable voice orchestration
  dialogue/              Typed text node and rule-based routing
  vehicle/               Explicit FakeVehicle climate / window simulation
  protocol/              Cockpit-domain Protobuf schemas
  scripts/               Dependency/model preparation and test runners
  tests/                 Agent contract and runner tests
plans/                   Implementation plan and module boundaries
docs/                    Repository-level migration records
```

Historical code remains migration input, not a production fallback. Capabilities
are tracked in the [migration ledger](runtime/docs/capability-migration.md).

## 🚀 Getting started

Use Linux/WSL with a C++17 compiler, CMake/Make, Protobuf + `protoc`, pkg-config
and libzmq development files. Preparation also needs curl, tar/bzip2 and SHA-256
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
cmake -S agent/apps/voice_demo -B runtime/build/voice-demo \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCABINFLOW_INFERENCE_DIR="$PWD/runtime/build/inference"
cmake --build runtime/build/voice-demo -j4
```

SDKs and downloaded LLM/TTS files are Git-ignored local artifacts, not included
in a fresh clone. Scripts pin sources and checksums. Runtime includes ZeroMQ
build targets even when the Demo uses in-process execution.

See the [voice application guide](agent/docs/voice-demo-integration.md) for the
backend/frontend launch commands, model paths, CLI results and verification. Inputs
must be complete **16 kHz mono PCM16 WAV** files; the full encoded message body
must fit within **4 MiB**, and each output directory must be new.

## 🧭 Roadmap

1. **Experience acceptance:** hands-on listening, fixed voice cases and cancellation demos.
2. **Showcase:** reproducible commands, scoped measurements and an operation video.

RAG, live recording/wake words, multi-occupant interaction, AAOS and hardware
optimization are deferred until the first interactive Demo is usable.

## 📚 Learn more

- [Implementation plan](plans/IMPLEMENTATION_PLAN.md) · [Directory plan](plans/RUNTIME_DIRECTORY_STRUCTURE.md)
- [Runtime architecture](runtime/docs/architecture.md) · [Build and development](runtime/docs/development.md)
- [Real-component integration and test evidence](agent/docs/voice-demo-integration.md)
- [Fixed-case results, timing boundaries and showcase checklist](agent/docs/voice-demo-acceptance-20261003.md)
- [Model versions and dependency audit](agent/docs/model-dependency-audit.md) · [Quality and known failures](agent/docs/demo-model-progress-20261002.md)
- [Runtime migration progress](runtime/docs/runtime-migration-closeout.md) · [Capability ledger](runtime/docs/capability-migration.md)
- [Fake voice demo](agent/docs/fake-voice-milestone.md): message/state validation with test audio, not real inference or a fallback.
