# ADR-005: Deterministic in-memory transport before ZeroMQ

## Status

Accepted and implemented for the initial Runtime integration boundary.

## Decision

`CabinFlow::TransportApi` exposes publish and subscribe operations over the
Protocol envelope. `CabinFlow::TransportInMemory` is its first implementation:

- `publish()` synchronously invokes handlers for the matching topic on the
  caller's thread; ordering among multiple matching subscribers is unspecified;
- callbacks are copied while the subscriber mutex is held, then invoked after
  the lock is released;
- a `Subscription` is owned through a `unique_ptr`; destruction calls
  `unsubscribe()`;
- a subscription may outlive its transport safely, but an in-flight concurrent
  publish may already have copied its callback.

The last point is the callback lifetime boundary: callers must keep data
captured by a callback valid until concurrent publishers are stopped or joined.
The later Runtime queue will provide a stronger dispatch lifecycle.

## Consequences

- Unit and integration tests can validate topic routing and subscription
  lifetime without sockets, sleeps, or an external service.
- This is not a ZeroMQ failure fallback. Apps choose one concrete transport at
  composition time.
- Ordering is only the synchronous order of each caller. Cross-thread ordering,
  bounded queues, and backpressure are deferred to subsequent Runtime work.
