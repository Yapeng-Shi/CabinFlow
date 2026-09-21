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
              Gateway

AgentProtocol <- DialogueTextNode -> Runtime
```

`Protocol` owns the domain message and its Protobuf codec. `TransportApi`
owns only publish/subscribe and subscription lifetime. `TransportInMemory` and
`TransportZmq` are mutually independent adapters; an app selects exactly one
when it composes its graph.

`Runtime` owns generic node lifecycle, `SessionLedger`, cancellation state,
and the first in-memory counter sink. It depends on the transport interface,
never a ZMQ header, endpoint, or socket. Business nodes receive these services
through `NodeContext`.

`CabinFlow::AgentProtocol` is a separate payload contract. It is linked only by
concrete Agent nodes and their integration tests: neither `Runtime` nor
`Gateway` depends on it.

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

The TCP data plane uses one framed path and no JSON fallback:

```text
TCP Framer -> RuntimeMessage decode -> Envelope validation
  -> target queue reservation -> SessionLedger admission -> queue commit
  -> TargetNode worker -> topic-specific payload decode
```

`TargetNode` accepts only a generic `Message`. The target owns payload topic
and Protobuf validation; `Runtime` owns each target's bounded queue and one
worker. A handled message has no success ACK. Rejection uses a typed
`runtime.delivery.error` message.

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

Each registered `TargetNode` has one Runtime-owned worker and one bounded queue.
Shutdown first stops admission, then discards queued work and joins workers,
then stops nodes. This prevents a stopped target from receiving a new message.
