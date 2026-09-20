# ADR-004: In-memory session ledger at the runtime admission boundary

## Status

Accepted and implemented in Day 2.

## Decision

The runtime admission boundary validates every `MessageEnvelope` before a
worker sees its payload. `SessionLedger` then maintains process-local state
for:

- schema compatibility, required identity fields, and TTL expiry;
- idempotency by `message_id`;
- monotonically increasing `sequence` within `(session_id, work_id, topic)`;
- final-message closure for the same stream; and
- cancellation scoped to one session or one `(session_id, work_id)` pair.

The ledger is thread-safe, but it only decides admission. Transport I/O and
business handlers retain their single-owner and queueing responsibilities from
ADR-002.

## Consequences

- A driver cancellation cannot suppress a passenger request that happens to
  use the same `work_id`.
- Streaming code must emit a unique `message_id`, a finite TTL, and a sequence
  number for every message.
- The first implementation is volatile and process-local. Reconnection,
  restart recovery, bounded retention, and distributed idempotency are not yet
  provided.
