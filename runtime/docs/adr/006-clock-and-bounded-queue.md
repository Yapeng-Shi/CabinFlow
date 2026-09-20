# ADR-006: Injected monotonic clock and reject-on-full queue

## Status

Accepted and implemented for the Runtime core.

## Decision

`SessionLedger` receives a `Clock` at construction and reads monotonic time
itself. Production composition uses `SteadyClock`; tests use `FakeClock` and
advance time without sleeping.

`BoundedQueue<T>` is a thread-safe, non-blocking FIFO. Its capacity is
mandatory and positive. `try_push()` returns `kFull` without replacing or
discarding an accepted item; it never waits for capacity.

Session/work cancellation remains owned by `SessionLedger`. The queue does not
create another cancellation store.

## Consequences

- Expiry tests are deterministic and do not depend on scheduler timing.
- Backpressure is visible to the caller, which must choose an explicit
  rejection, retry, or cancellation policy when a future Runtime submits work.
- Blocking worker dispatch and queue wake-up policy are intentionally deferred
  until the Runtime lifecycle is introduced.
