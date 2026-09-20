# ADR-009: Runtime owns node lifecycle and dual-scope cancellation

## Status

Accepted and verified on the WSL x86 development environment.

## Decision

`CabinFlow::Runtime` owns a set of generic `Node` instances and gives each
node a `NodeContext` rather than letting applications construct per-node
transport, ledger, and logger dependencies independently.

- Nodes are registered before `start()`. Runtime starts them in registration
  order and stops them in reverse order.
- If any node fails to start, Runtime stops only the nodes that had previously
  started and returns `kNodeStartFailure`; it does not leave a partial graph
  running or attempt a second implementation as fallback.
- Node registration after Runtime has started is rejected. This keeps the
  dependency graph and its shutdown ordering explicit for the first version.
- Runtime owns both `SessionLedger` and `CancellationRegistry`.
  `cancel_session` and `cancel_work` update both in one API call.
- Runtime also owns the first `InMemoryMetrics` sink. Lifecycle, cancellation,
  and demo message results increment concrete counters without introducing an
  external monitoring dependency.
- `SessionLedger` rejects future delivery at the admission boundary.
  `CancellationToken`, scoped by `(session_id, work_id)`, lets an in-flight
  node observe the same monotonic cancellation state at its own safe point.

## Consequences and boundaries

- Cancellation has no reset operation. A new conversational turn must receive
  a new `work_id`; reusing an old work ID after cancellation is invalid.
- This Runtime does not automatically translate a network `MessageKind::kCancel`
  into a local cancel call yet. The future control-plane node must validate the
  envelope and explicitly invoke Runtime cancellation, so protocol authority
  is not hidden in the transport adapter.
- A node that starts asynchronous work must make `stop()` wait for, cancel, or
  otherwise make its own callbacks safe before returning. Runtime provides
  ordering, not a generic solution for arbitrary business-thread ownership.
- `runtime_test` verifies failure rollback, reverse shutdown, the immutable
  post-start graph, and one cancellation reaching both token and ledger.
  `cancellation_test` verifies session/work isolation and idempotency;
  `metrics_test` verifies the in-memory counter behavior.
