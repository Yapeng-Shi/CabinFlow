# 🚘 CabinFlow

### Offline voice interaction. A model-independent C++ foundation.

🌐 **English** · [简体中文](README.zh-CN.md)

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)
![Linux](https://img.shields.io/badge/Linux-WSL2-333333?logo=linux)
![Protobuf](https://img.shields.io/badge/Protocol-Protobuf-4285F4)
![Local inference](https://img.shields.io/badge/Inference-Local_CPU-2E7D32)
![Project stage](https://img.shields.io/badge/Stage-CLI_Integration-orange)

CabinFlow is an **offline intelligent-cockpit voice Agent** built on a reusable
C++ Runtime. It brings local speech recognition, intent routing, language-model
inference and speech synthesis into a testable application pipeline.

The key idea: **separate the system from its models**. Communication, task
identity, queues, workers and cancellation belong to Runtime; algorithms belong
to Agent components. Models can evolve without rebuilding the entire application.

> 🚧 Active development: real-model CLI integration is available on x86 CPU /
> WSL2 / Ubuntu 24.04. The interactive Qt/TCP Demo and target-hardware validation
> are not complete.

## 💡 Why CabinFlow?

A voice assistant needs more than model calls: it must connect stages, correlate
results with the right task, bound concurrent work, handle failures and shut down
safely. CabinFlow focuses on making that foundation explicit and reusable.

- **Offline by design:** inference uses local models, not cloud services. First-time dependency/model preparation requires network access.
- **Replaceable components:** ASR, LLM and TTS use small C++ interfaces with constructor injection; model SDKs stay inside adapters.
- **Runtime-managed execution:** each target has its own bounded queue and single worker, with admission, lifecycle and backpressure handled centrally.
- **Traceable tasks:** Protobuf messages carry trace, session, work and message identities, sequence, TTL and final markers.
- **Explicit failure and cancellation:** failures stop downstream processing; cancellation suppresses late results and waits for actual handler completion.
- **Testable boundaries:** contract and integration tests cover protocols, task isolation, queues, lifecycle and component behavior.

No automatic model fallback, legacy-format guessing or plugin framework is added
to the current pipeline.

## 🏗️ Architecture

```text
               Agent: application and algorithms

WAV ──→ ASR ──→ Dialogue / Router ──→ LLM ──→ TTS ──→ Answer WAV
Text ─────────→         │                     ↑
                        └─ Known intent ──────┘
                           explicit receipt; no vehicle execution yet

                 Runtime: shared execution foundation
┌──────────────────────────────────────────────────────────────────┐
│ Message identity · Work lifecycle · Ledger · Bounded queues       │
│ Per-target workers · Cancellation · Transport · Observability     │
└──────────────────────────────────────────────────────────────────┘

Current entry: CLI
Next entry:    Qt/QML → TCP Gateway → the same application pipeline
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
| Planned interface | Qt/QML — not yet a verified UI/playback capability |

## 📍 Current scope

- **Implemented:** generic Runtime, typed TCP control/text paths, injected inference adapters and reusable `VoicePipeline` orchestration.
- **Executed:** real text/WAV inputs traversing Runtime nodes and producing non-silent answer WAV files.
- **Next:** connect the real pipeline to TCP/Qt, add playback and user-operated cancellation, then FakeVehicle air-conditioning simulation.
- **Not claimed:** complete voice UX, overall model accuracy, production vehicle control, AAOS integration or physical RK3576 deployment.

The CLI currently supports one active task and non-streaming output. It writes
audio files rather than playing them. `VoicePipeline::cancel()` exists, but the
CLI has no cancel control. Model-quality issues and human-listening gaps remain
recorded separately; successful execution is not semantic-quality acceptance.

## 📂 Project layout

```text
runtime/                 Generic execution, network, Gateway and Transport
agent/
  apps/voice_demo/       Real-model CLI and application assembly
  inference/             Backend interfaces and native SDK adapters
  pipeline/              Reusable voice orchestration
  dialogue/              Typed text node and rule-based routing
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

**Prepare and build the real-model CLI:**

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

See the [real-model CLI guide](agent/docs/voice-demo-integration.md) for exact
model paths, text/WAV commands, result files and verification records. Inputs
must be complete **16 kHz mono PCM16 WAV** files; the full encoded message body
must fit within **4 MiB**, and each output directory must be new.

## 🧭 Roadmap

1. **Interactive loop:** TCP/Qt input, transcript/answer display and playback.
2. **Task control:** visible cancellation and cleanup, with stale results suppressed.
3. **Cockpit simulation:** explicitly labeled FakeVehicle state and repeatable demo cases.

RAG, live recording/wake words, multi-occupant interaction, AAOS and hardware
optimization are deferred until the first interactive Demo is usable.

## 📚 Learn more

- [Implementation plan](plans/IMPLEMENTATION_PLAN.md) · [Directory plan](plans/RUNTIME_DIRECTORY_STRUCTURE.md)
- [Runtime architecture](runtime/docs/architecture.md) · [Build and development](runtime/docs/development.md)
- [Real-component integration and test evidence](agent/docs/voice-demo-integration.md)
- [Model versions and dependency audit](agent/docs/model-dependency-audit.md) · [Quality and known failures](agent/docs/demo-model-progress-20261002.md)
- [Runtime migration progress](runtime/docs/runtime-migration-closeout.md) · [Capability ledger](runtime/docs/capability-migration.md)
- [Fake voice demo](agent/docs/fake-voice-milestone.md): message/state validation with test audio, not real inference or a fallback.
