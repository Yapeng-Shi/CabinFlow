# ADR-003: Identity and cancellation scope

## Status

Accepted for implementation in week one.

## Decision

- `trace_id` identifies one end-to-end user request.
- `session_id` identifies one occupant conversation.
- `work_id` identifies one task belonging to a session; its owning Unit manages
  lifecycle, while internal stages may cross TargetNodes under that same work.
- `message_id` identifies one message.
- `sequence` is monotonic within `(session_id, work_id, topic)`.

Cancellation targets a session or a work item. It must not affect other
sessions, even when they use the same node implementation.
Every external TCP data target must match that work's owning Unit. This check
precedes queue/Ledger admission; it does not restrict internal stage routing.
The real single-turn Agent creates a fresh work for each complete input and
uses explicit input message identity to return its final result to the original
connection, not a permanent connection-to-work ownership assumption.

## Consequences

- Streaming buffers and output sequence counters cannot be global static
  variables.
- Duplicate, stale, and post-final messages can be detected deterministically.
- Structured logging uses all three primary IDs for every request event.
