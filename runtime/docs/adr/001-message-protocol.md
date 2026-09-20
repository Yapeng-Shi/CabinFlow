# ADR-001: Versioned runtime envelope

## Status

The C++ metadata contract, Protobuf schema, and codec are implemented in
`CabinFlow::Protocol` and covered by `message_codec_test`.

## Decision

All new runtime messages will use a versioned envelope. The envelope owns
identity, ordering, delivery bounds, source and target metadata. Business
payloads remain in `protocol::Message`, separate from transport metadata.

The C++ envelope currently includes:

- `schema_version`
- `message_id`
- `trace_id`
- `session_id`
- `work_id`
- `source_node`, `target_node`, and `topic`
- `sequence`, `created_monotonic_ns`, and `ttl_ms`
- `is_final` and `kind` (`data`, `cancel`, or `error`)

The Protobuf schema carries this metadata and a byte payload without changing
Runtime ownership rules. Agent-specific typed payload and error schemas remain
outside Runtime.

## Consequences

- Legacy raw strings such as sentinel end markers will be confined behind an
  adapter when their transport migration starts.
- Unsupported schema versions and expired messages fail explicitly.
- Ordering and cancellation are defined per session and work stream, rather
  than through process-global state.
