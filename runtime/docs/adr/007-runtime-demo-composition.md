# ADR-007: One deterministic Runtime demo

## Status

Accepted and implemented for the initial end-to-end composition path.

## Decision

`runtime_demo` composes `SteadyClock`, `SessionLedger`, `ConsoleLogger`, and
`InMemoryTransport` in `main.cpp`. `TextSourceNode` publishes explicit
`MessageEnvelope` inputs; `EchoProcessorNode` subscribes, asks the Ledger for
admission, and logs either the echoed result or a typed rejection state.
One invocation demonstrates two concurrent sessions, duplicate ID, input
after final, stale sequence, expiry, work cancellation, and another unaffected
work. Source `published` means only local Transport delivery, not Ledger success.

The demo remains a two-node flow; each in-memory publish invokes its callback
synchronously, while the two concurrent publishers can interleave. It does not stand in
for ASR, LLM, TTS, a vehicle service, a worker queue, or a ZeroMQ deployment.

## Consequences

- `runtime_demo_integration` executes the same binary and asserts the
  trace/session/work/message identity and outcome of every stated scenario.
- Console logs carry Unix wall-clock milliseconds for cross-process search;
  deadline decisions still use monotonic time.
- Only the app chooses `InMemoryTransport`; Runtime and Protocol remain
  independent of the concrete transport.
- Callback lifetime is explicit: `EchoProcessorNode::stop()` destroys its
  subscription before the transport is destroyed.
