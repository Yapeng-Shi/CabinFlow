# ADR-006: Injected monotonic clock and reject-on-full queue

## Status

Accepted and implemented for the Runtime core.

## Decision

`SessionLedger` receives a `Clock` at construction and reads monotonic time
itself. Production composition uses `SteadyClock`; tests use `FakeClock` and
advance time without sleeping.

`BoundedQueue<T>` is a thread-safe, non-blocking FIFO. Its capacity is
mandatory and positive. `try_push()` returns `kFull` without replacing or
discarding an accepted item; it never waits for capacity. A caller can reserve
one slot before it mutates `SessionLedger`, then atomically commit or release
that slot. Therefore a queue-full rejection cannot leave a duplicate or
sequence record behind.

Session/work cancellation remains owned by `SessionLedger`. The queue does not
create another cancellation store.

## Consequences

- Expiry tests are deterministic and do not depend on scheduler timing.
- Backpressure is visible to the Gateway as a typed `QUEUE_FULL` rejection;
  it does not evict older work or retry automatically.
- Runtime owns one queue and one worker per `TargetNode`. Workers recheck TTL
  before calling the node, so an item that expired while waiting is rejected
  without invoking business code.
