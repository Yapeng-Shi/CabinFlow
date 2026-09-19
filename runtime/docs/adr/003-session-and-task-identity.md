# ADR-003: Identity and cancellation scope

## Status

Accepted for implementation in week one.

## Decision

- `trace_id` identifies one end-to-end user request.
- `session_id` identifies one occupant conversation.
- `work_id` identifies one node task belonging to a session.
- `message_id` identifies one message.
- `sequence` is monotonic within `(session_id, work_id, topic)`.

Cancellation targets a session or a work item. It must not affect other
sessions, even when they use the same node implementation.

## Consequences

- Streaming buffers and output sequence counters cannot be global static
  variables.
- Duplicate, stale, and post-final messages can be detected deterministically.
- Structured logging uses all three primary IDs for every request event.
