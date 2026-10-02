# CabinFlow Runtime development

## Runtime build and test

The monorepo root has no CMake entry point. Run the canonical CMake presets
from `runtime/`; historical modules remain migration input until their
dependencies and behavior have been characterized.

From the WSL checkout, run:

```bash
cd /home/projects/CabinFlow/runtime
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

The same workflow is available from `runtime/` through:

```bash
./scripts/build.sh
./scripts/test.sh
```

The generated files are placed under `build/`, which is already ignored by Git.

## Historical baseline and migration input

The standalone Runtime trees are retained as migration input but are not an
active build path. `baseline.md` records the original dependency observation;
`capability-migration.md` records the test-first decision and deletion
boundary for each legacy capability.
[`legacy-dependencies.md`](legacy-dependencies.md) records the old installer
recipe and current dependency mapping. Its inventory test is static evidence,
not proof that missing eventpp/simdjson installer scripts are reproducible.

## Runtime core and lifecycle

`CabinFlow::Protocol` provides the transport-neutral `MessageEnvelope` and
message kinds. `CabinFlow::Runtime` owns the in-memory `SessionLedger`, a
session/work `CancellationRegistry`, and a fixed node graph. Nodes receive a
`NodeContext` with transport, clock, ledger, cancellation-token, logger, and
in-memory counter access. Runtime starts nodes in registration order and stops
them in reverse; startup failure rolls back the already started prefix. The
demo uses this composition path rather than directly starting its processor
node.

Runtime is a one-shot instance. Repeated start while Starting/Running returns
`kAlreadyStarted`; start after failure or stop returns `kLifecycleEnded`.
Stop before first start also consumes the instance. Repeated external stop is
idempotent and waits for any current startup/cleanup to finish. Lifecycle state
changes and admission share one lock, but node callbacks, queue cleanup and
worker joins run outside it. `runtime.stop_waits` counts selected waiting paths,
not elapsed time. Worker/callback-originated stop is still an unapproved boundary.
See [`runtime-migration-closeout.md`](runtime-migration-closeout.md) for the
2026-10-02 tests and unverified paths.

The ledger validates TTL and identity, then protects per-stream ordering,
duplicate suppression, final-message closure, and scoped cancellation. Runtime
cancellation also updates in-flight `CancellationToken`s, so an active node
can observe cancellation at a safe point. The behavior is exercised by
`session_ledger_test`, `cancellation_test`, and `runtime_test`; see ADR-004
and ADR-009 for the ownership boundaries.

## In-memory transport

`CabinFlow::TransportInMemory` implements the public transport API for tests
and local wiring. It delivers matching topics synchronously on the publisher's
thread and uses an RAII subscription handle. The behavior and invalid-input
results are covered by `in_memory_transport_test`; see ADR-005 for callback
lifetime and concurrency boundaries.

## ZeroMQ transport

`CabinFlow::TransportZmq` is available when the host provides `libzmq` and
Protobuf. Applications compose it explicitly through `ZmqTransportConfig`;
there is no runtime fallback to the in-memory implementation. The adapter uses
a dedicated socket-owning I/O thread plus a callback thread, and its TCP
loopback integration test verifies Protobuf metadata, topic routing, RAII
unsubscribe, and callback-thread separation:

```bash
ctest --preset linux-debug --tests-regex zmq_transport_test
```

`publish()` reports only the local encoding and ZeroMQ send result. PUB/SUB
does not acknowledge remote processing. See ADR-008 for the lifecycle and
slow-joiner boundary.

## Clock and backpressure core

`SessionLedger` consumes an injected monotonic `Clock`; the test suite uses a
`FakeClock` rather than sleeping to check TTL expiry. `BoundedQueue<T>` is a
thread-safe FIFO with a reject-on-full policy. It is covered by
`bounded_queue_test`; see ADR-006 for its explicit non-blocking boundary.
For each target, `Runtime::target_stats(name)` exposes the current pending
queue depth, full-queue rejection count, handler-call count, and total handler
duration in nanoseconds. The snapshot is read-only and is not an atomic view
across all four fields. `target_runtime_contract_test` checks these values
with a blocked consumer and an injected clock.

The larger producer/consumer check is intentionally outside default CTest.
Build and run it explicitly when changing queue synchronization:

```bash
cmake --build build/linux-debug --target bounded_queue_stress_test
./build/linux-debug/tests/bounded_queue_stress_test
```

## Typed TCP data plane

`TargetNode` is Runtime's generic data-plane interface. Runtime owns a unique
target-name mapping, a bounded queue, and one worker for each target. Gateway
reserves a target slot before it admits the envelope to `SessionLedger`; only
an accepted envelope is committed to the queue. Payload parsing remains in the
Agent node, so Runtime and Gateway do not link `CabinFlow::AgentProtocol`.

The first Agent path is `cockpit.text.input -> dialogue.primary ->
cockpit.text.output`. It uses a
Protobuf `TextInput`; empty, malformed, invalid UTF-8, or unsupported-topic
messages receive typed `runtime.delivery.error` responses. Successful handling
does not receive an ACK. A final input produces a typed `TextOutput` on the
TCP connection that sent that exact input message; this rule-based recognition
does not execute vehicle controls or real ASR/LLM/TTS. Control requests use
the client's and Gateway's shared monotonic clock domain for TTL validation.
The direct Runtime, response-contract, and real TCP
checks are included in default CTest as `target_runtime_contract_test`,
`delivery_error_contract_test`, and `data_plane_gateway_tcp_test`.

## Runtime demo

Run the deterministic two-node example after a build:

```bash
./build/linux-debug/apps/runtime_demo/runtime_demo
```

The two-node demo selects `InMemoryTransport` and emits structured logs with
wall-clock time, node, trace/session/work/message identity, status, and detail.
One invocation shows two concurrent sessions, duplicate input, stale sequence,
deadline, work cancellation with an unaffected sibling work, and post-final
rejection. `runtime_demo_integration` asserts every scenario from the same
binary; its output is not a real model or TCP Gateway demonstration.

## Sanitizers

Use the `linux-asan` preset for the checked AddressSanitizer configuration.
ThreadSanitizer evidence is limited to selected historical network/Gateway
tests, not the full Runtime; see [`sanitizers.md`](sanitizers.md).
