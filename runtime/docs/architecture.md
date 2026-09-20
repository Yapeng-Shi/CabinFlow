# CabinFlow Runtime architecture

## Dependency direction

```text
Protocol
   ^
   |
TransportApi      Observability
   ^                    ^
   |                    |
InMemory / Zmq          |
          \             |
           \            |
              Runtime
                 ^
                 |
               apps
```

`Protocol` owns the domain message and its Protobuf codec. `TransportApi`
owns only publish/subscribe and subscription lifetime. `TransportInMemory` and
`TransportZmq` are mutually independent adapters; an app selects exactly one
when it composes its graph.

`Runtime` owns generic node lifecycle, `SessionLedger`, cancellation state,
and the first in-memory counter sink. It depends on the transport interface,
never a ZMQ header, endpoint, or socket. Business nodes receive these services
through `NodeContext`.

## Message and cancellation flow

```text
source node
  -> MessageEnvelope + payload
  -> ITransport.publish
  -> transport routing
  -> node handler
  -> SessionLedger admission
  -> business processing
```

For ZeroMQ the transport routing step has two frames: `topic` and a
Protobuf-encoded `RuntimeMessage`. The receiver decodes and verifies that both
topic values agree before dispatching a handler.

Cancellation takes a parallel control path:

```text
Runtime.cancel_session/work
  -> SessionLedger rejects later delivery
  -> CancellationToken reports cancelled to in-flight work
```

The scope is always `(session_id, work_id)`, except session cancellation which
intentionally covers every work item in that session. There is no reset; a new
turn requires a new work ID.

## Thread ownership

`InMemoryTransport` invokes handlers synchronously on the publishing thread.
`TransportZmq` uses one internal I/O thread to own ZMQ sockets and a separate
callback thread for user handlers. The ZMQ adapter reports local send outcome,
not remote processing acknowledgement. See ADR-005, ADR-008, and ADR-009 for
the exact lifecycle boundaries.
