# ADR-002: Single-owner transport threads

## Status

Accepted for implementation in week one.

## Decision

Each ZeroMQ socket is owned by exactly one I/O thread. Business callbacks never
read from or write to the socket directly. The I/O thread validates and
deserializes an incoming message, then posts it to a bounded worker queue.

Requests must carry a deadline. A timeout or cancellation resolves the request
with a structured failure instead of waiting indefinitely.

## Consequences

- Cross-thread send/receive on the same socket is prohibited.
- Queue capacity and overflow policy are explicit runtime configuration.
- Slow handlers cannot block the transport I/O loop.
