# CabinFlow-Runtime Project Instructions

## Purpose

- This repository is the C++ runtime and communication foundation for an offline intelligent-cockpit voice agent.
- Development serves both a working system and preparation for C++ infrastructure, Linux, edge-AI, and intelligent-cockpit roles.
- Optimize for knowledge the user can explain and defend in an interview, backed by source, tests, logs, and measured results.

## Scope

- Own transport abstraction, message contracts, node lifecycle, session/work isolation, cancellation, backpressure, observability, and failure handling.
- Do not implement ASR, RAG, LLM, TTS, or cockpit business logic here unless it is a minimal test double.
- Prefer C++17/20, CMake, focused unit tests, and explicit ownership/lifetime rules.

## Change policy

- The project is under active development and does not promise backward compatibility unless the user explicitly says otherwise.
- Perform direct migrations: update all in-repository callers and remove old APIs in the same change.
- Do not add compatibility adapters, deprecated wrappers, dual protocols, fallback implementations, silent defaults, or speculative plugin systems.
- Preserve existing user modifications and avoid unrelated cleanup.
- Keep one message identity model and document the distinct roles of `trace_id`, `session_id`, and `work_id`.

## Learning focus

For substantial changes, teach the user:

- thread and socket ownership;
- object and callback lifetime;
- Reactor/event-loop behavior;
- IPC and transport boundaries;
- ordering, idempotency, cancellation, and backpressure;
- error propagation and resource cleanup;
- how to reproduce, observe, and test a failure.

Explain mechanisms in the response or requested docs, not through redundant production code.

## Verification and job evidence

- Add a focused regression test for behavior changes and bug fixes.
- Prefer deterministic fake nodes and fault injection over unrepeatable manual demonstrations.
- Record the command, environment, sample size, concurrency, timing boundary, and P50/P95 for performance claims.
- After a meaningful change, summarize: problem, root cause, solution, verification, remaining boundary, and a concise interview takeaway.
- Never claim this public project reproduces NIO proprietary SkyOS or NOMI internals.

## Platform boundary

- Current functional validation is on x86 Linux/WSL unless a result explicitly states otherwise.
- RK3576/RKNN/RKLLM work is a target backend or deployment preparation until verified on physical hardware.
- Cross-compilation, mocks, and simulators are not hardware deployment evidence.
