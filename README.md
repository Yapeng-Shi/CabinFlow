# CabinFlow

CabinFlow is an in-progress, offline intelligent-cockpit voice-agent runtime.
It combines a C++ communication/runtime foundation with local ASR, RAG, LLM,
and TTS components. The development target is a multi-occupant cockpit agent
with session isolation, structured tool calls, and vehicle-service adapters.

## Repository layout

```text
runtime/  C++ runtime, communication foundation, lifecycle, and tests
agent/    ASR, RAG, local LLM, TTS, and application-level components
docs/     Repository-level design and migration records
```

## Current status

The Runtime foundation's canonical CMake entry point is `runtime/`, not the monorepo root. The
current C++ foundation has a transport-neutral envelope, a typed Protobuf TCP
Gateway, control/work lifecycle, target-specific bounded queues, and
session/work admission and cancellation. The two-node in-memory demo exercises
identity, concurrent sessions, ordering, expiry, final, and cancellation.
The Agent's typed text path recognizes a small set of cockpit intents and
returns text to the originating TCP client; it does not execute vehicle
controls or run ASR/LLM/TTS models.

Week 2 now has a separate **fake-only** WAV-to-WAV demonstration: a fixed
silent fixture passes through Runtime-managed ASR, rule-router, LLM, and TTS
targets and produces a deterministic test tone, not spoken dialogue. Run it
with `bash agent/scripts/run_fake_voice.sh`; scope and remaining work are in
[`agent/docs/fake-voice-milestone.md`](agent/docs/fake-voice-milestone.md).

Historical Runtime and Agent modules remain migration input, not alternative
production paths. The 2026-09-29 x86/WSL Debug and ASan suites each passed
26/26 CTest cases; the exact first-week scope is recorded in
[`runtime/docs/week1-acceptance.md`](runtime/docs/week1-acceptance.md).

The current priority is the decoupled, single-session Demo in
[`the revised plan`](plans/IMPLEMENTATION_PLAN.md), not full Runtime migration
closeout or continued model-specific diagnostics. Agent-owned ASR/LLM/TTS ports,
native Sherpa/llama adapters, reusable orchestration and a real CLI are now
implemented. Its separate product CMake entry is `agent/apps/voice_demo/`;
Runtime does not depend on model SDKs. One real text-to-LLM-to-TTS execution
returned Beijing and generated a non-silent WAV. Build/run instructions and
the current integration evidence are in
[`the CLI integration record`](agent/docs/voice-demo-integration.md).

Qwen3-0.6B non-thinking still fails the original arithmetic answer check,
including an independent original-weight diagnostic. The earlier five-fixture
ASR reference run had only 1/5 exact matches. These quality failures remain
recorded in [`the model record`](agent/docs/demo-model-progress-20261002.md);
they do not block component integration and have not been relabeled as passes.

The earlier 2026-10-02 one-shot external lifecycle fix passed Debug/ASan 29/29
and two lifecycle TSan tests. This does not complete legacy dependency
installation or remaining migration capabilities; see the
[`historical closeout record`](runtime/docs/runtime-migration-closeout.md).

TCP/Qt integration of the real model chain, playback, FakeVehicle, human
listening and final Demo acceptance remain unfinished. The formal daemon,
AAOS integration and RK3576 hardware verification remain future work. RKNN/RKLLM code is a target-platform
integration path, not physical-device deployment evidence.

## Build the runtime foundation

```bash
cd runtime
./scripts/test.sh
```

See [`runtime/docs/development.md`](runtime/docs/development.md) for the
current build boundary and [`docs/MIGRATION.md`](docs/MIGRATION.md) for the
initial import policy.
