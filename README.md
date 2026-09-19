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

The initial monorepo migration preserves the two original working trees while
excluding generated build and install artifacts. The runtime foundation has a
root CMake scaffold and a build-sanity test; existing legacy modules are
integrated incrementally as their dependencies and tests are made reproducible.

No RK3576 hardware performance claim is made in this repository. RKNN/RKLLM
code is treated as a target-platform integration path pending physical-device
validation.

## Build the runtime foundation

```bash
cd runtime
./scripts/test.sh
```

See [`runtime/docs/development.md`](runtime/docs/development.md) for the
current build boundary and [`docs/MIGRATION.md`](docs/MIGRATION.md) for the
initial import policy.
