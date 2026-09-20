# ADR-007: One deterministic Runtime demo

## Status

Accepted and implemented for the initial end-to-end composition path.

## Decision

`runtime_demo` composes `SteadyClock`, `SessionLedger`, `ConsoleLogger`, and
`InMemoryTransport` in `main.cpp`. `TextSourceNode` publishes one text message;
`EchoProcessorNode` subscribes to it, asks the Ledger for admission, and logs
the echoed result.

The demo is intentionally a two-node, synchronous flow. It does not stand in
for ASR, LLM, TTS, a vehicle service, a worker queue, or a ZeroMQ deployment.

## Consequences

- `runtime_demo_integration` executes the same binary and asserts the
  trace/session/work/message correlation fields in its structured output.
- Only the app chooses `InMemoryTransport`; Runtime and Protocol remain
  independent of the concrete transport.
- Callback lifetime is explicit: `EchoProcessorNode::stop()` destroys its
  subscription before the transport is destroyed.
