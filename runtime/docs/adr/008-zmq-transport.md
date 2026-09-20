# ADR-008: ZeroMQ transport uses a dedicated I/O owner and callback dispatcher

## Status

Accepted and verified on the WSL x86 development environment.

## Decision

`CabinFlow::TransportZmq` is the production-shaped transport adapter for the
Runtime boundary. It implements the existing `ITransport` API without exposing
ZeroMQ headers to `core/`, `agent/`, or application nodes.

- A configured `bind_endpoint` owns one `ZMQ_PUB` socket; a configured
  `connect_endpoint` owns one `ZMQ_SUB` socket. A transport can be send-only,
  receive-only, or own both independently.
- Each published message is exactly two ZeroMQ frames: the UTF-8 topic first,
  then the Protobuf-encoded `RuntimeMessage`. The receiver rejects a message
  whose decoded envelope topic does not match the routing frame.
- One internal I/O thread creates, configures, polls, and destroys every ZMQ
  socket and context. Public `publish`, `subscribe`, and `unsubscribe` calls
  submit a command to that owner and receive its local outcome.
- A separate callback thread invokes handlers after the I/O thread has decoded
  and routed the message. This prevents handler code from touching a ZeroMQ
  socket and permits a handler to call the transport API again.
- `Subscription` remains RAII. Unsubscribe removes the handler before the
  caller returns; an already queued callback may still run.

## Consequences and boundaries

- A successful `publish` means the local adapter encoded the message and
  accepted it into ZeroMQ. PUB/SUB has no peer acknowledgement, so it is not a
  remote-delivery guarantee. There is deliberately no hidden retry or fallback
  transport.
- Subscribers have the ZeroMQ "slow joiner" boundary: a publisher can emit
  before the subscriber's filter reaches it. The integration test makes its
  handshake wait explicit instead of hiding retries in production code.
- A malformed multipart message, malformed Protobuf payload, or topic mismatch
  is discarded at the transport boundary. It never reaches a business handler.
- Handler exceptions are not swallowed. An exception terminates the process by
  normal C++ thread rules rather than presenting failed processing as success.
- The adapter was compiled and tested using libzmq 4.3.5 and Protobuf 3.21.12
  in WSL x86. This is not evidence of an RK3576 deployment or its performance.
