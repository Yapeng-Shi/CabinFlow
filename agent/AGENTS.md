# CabinFlow-Agent Project Instructions

## Purpose

- This repository implements an offline multi-occupant intelligent-cockpit voice agent on top of CabinFlow-Runtime.
- Development serves both a working system and preparation for intelligent-cockpit, edge-AI, LLM-inference, and C++ roles.
- Every major feature should create knowledge and evidence the user can reproduce and defend in an interview.

## Scope

- Own ASR, dialogue routing, RAG, local LLM, streaming TTS, vehicle-signal access, safe tool calling, scene orchestration, and end-to-end evaluation.
- Keep communication, lifecycle, and generic session mechanics in CabinFlow-Runtime rather than duplicating them here.
- Use deterministic WAV input/output and fake vehicle signals before adding hardware-dependent paths.

## Change policy

- Use one canonical behavior and data path.
- Perform direct migrations and update every in-repository caller in the same change.
- Do not add fallback to old models or old protocols, compatibility aliases, dual reads/writes, silent defaults, or mock success presented as real inference.
- Do not add frameworks, models, protocols, or abstractions solely for resume keywords.
- Keep production code concise; put teaching material in responses, tests, or requested docs.

## Learning focus

For substantial changes, teach the user:

- the ASR -> router -> RAG/LLM -> sentence chunking -> TTS data flow;
- state ownership across listening, recognizing, thinking, speaking, cancellation, and completion;
- retrieval, routing, inference, and playback latency boundaries;
- safe Function Call validation and separation from vehicle control;
- failure reproduction, logs, root-cause isolation, and regression testing;
- why the selected design is appropriate and what trade-off it makes.

## Verification and job evidence

- Distinguish implemented, enabled, measured, simulated, target-only, and upstream capabilities.
- Metrics must include device, OS, model/version, input set, sample count, concurrency, timing boundary, and statistic.
- For major work, record: difficulty, reproduction, evidence, root cause, solution, test, result, and remaining boundary.
- Resume wording must describe the user's actual contribution and verified environment.
- Never claim NIO proprietary implementation details; describe the project as informed by public intelligent-cockpit requirements.

## Platform boundary

- Current functional and performance results are x86 Linux/WSL results unless explicitly labeled otherwise.
- AAOS Emulator, fake VHAL, `vcan`, mocks, and cross-compilation are simulation or preparation, not production-vehicle validation.
- RK3576/RKNN/RKLLM is unverified until exercised on physical hardware with recorded versions and metrics.
