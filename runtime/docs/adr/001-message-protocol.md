# ADR-001: Versioned runtime envelope

## Status

Accepted for implementation in week one; the Protobuf schema follows in the
protocol task.

## Decision

All new runtime messages will use a versioned envelope. The envelope owns
identity, ordering, delivery bounds, source and target metadata. Business
payloads remain separate from transport metadata.

The first schema will include:

- `schema_version`
- `message_id`
- `trace_id`
- `session_id`
- `work_id`
- `source_node`, `target_node`, and `topic`
- `sequence`, `created_monotonic_ns`, and `ttl_ms`
- `is_final`
- a typed payload or structured error

## Consequences

- Legacy raw strings such as sentinel end markers are confined behind an
  adapter during migration.
- Unsupported schema versions and expired messages fail explicitly.
- Ordering and cancellation are defined per session and work stream, rather
  than through process-global state.
