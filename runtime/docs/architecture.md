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

The in-process two-node demo uses `ITransport` directly. Its subscription
callback performs Ledger admission before business processing; it is not the
TCP Gateway's target queue path:

```text
TextSourceNode -> ITransport.publish -> EchoProcessorNode subscription callback
  -> SessionLedger admission -> echo processing
```

For ZeroMQ the transport routing step has two frames: `topic` and a
Protobuf-encoded `RuntimeMessage`. The receiver decodes and verifies that both
topic values agree before dispatching a handler.

The TCP data plane uses one framed path and no JSON fallback:

```text
TCP Framer -> RuntimeMessage decode -> Envelope/work-owner validation
  -> target queue reservation -> SessionLedger admission -> queue commit
  -> TargetNode worker -> topic-specific payload decode
```

The current typed Agent path is `cockpit.text.input -> dialogue.primary ->
cockpit.text.output`. Gateway routes the final output to the TCP connection
associated with that input's `(session_id, message_id)`; a work is not bound
permanently to a connection. This rule-based path recognizes cockpit intents
but does not execute vehicle controls or run ASR/LLM/TTS models.

For the Week 2 fake voice experiment, the user approved preserving one work
while internal messages cross `asr.primary`, `dialogue.primary`, `llm.fake`,
and `tts.fake` TargetNodes. The CLI validates typed register/setup/exit
commands through `ControlEnvelopeValidator` and `ControlService`, creates
the work for `asr.primary`, and submits each typed data stage through Runtime
reservation and Ledger admission.
That experiment is not a TCP audio ingress path or a real model pipeline.
Gateway now checks every external data target against the work-owning Unit.
`data_plane_gateway_tcp_test` verifies rejection before Ledger mutation, a valid
resubmission, and preserved internal cross-Target delivery under the same work.

The separate real-model Agent app composes this Gateway with `VoicePipeline`.
Its text/WAV root waits for downstream Runtime completions and emits one
`cockpit.task.result` after cleanup. Gateway's `DataTaskHooks` reserves/rolls back
one application task slot and obtains an opaque final output at root completion;
neither Runtime nor Gateway parses `VoiceTaskResult` or links AgentProtocol.
Pre-admission errors remain DeliveryError; accepted-task failures become the
application's final result, avoiding two competing terminal notifications.
This app drains its root before stopping Runtime: stopping while a root waits
for queued downstream work would discard that queue and strand the root.
See the [Agent integration guide](../../agent/docs/voice-demo-integration.md).

Control requests use a separate admission path:

```text
TCP Framer -> ControlEnvelopeValidator -> ControlService -> UnitRegistry
```

`Setup` creates a server-side work ID without entering the data Ledger.
Control TTL is checked before the service using the same monotonic clock domain
as the client. A successful `Exit` explicitly cancels the work in Runtime
before the success response is sent.

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

Runtime is one-shot: failed startup and stop are terminal, not restart points.
Starting freezes registration, and Stopping closes admission under the lifecycle
lock. Node callbacks and worker joins run outside that lock so worker queries
cannot form a lock/join cycle. Concurrent external stop callers wait for the
single cleanup owner; terminal state is published only after cleanup. Direct
stop from a worker or lifecycle callback is still a pending decision, not an
implemented asynchronous shutdown path. See ADR-009 and the migration closeout
record for the tested boundary.
