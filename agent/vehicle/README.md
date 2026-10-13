# vcan vehicle backend

`SocketCanVehicle` is the production Demo backend. It opens only `vcan0`, owns its
SocketCAN descriptor, and never substitutes FakeVehicle. `FakeVehicle` implements
the same interface in the test-only `CabinFlow::TestVehicle` target. Both report `simulated=true`; neither
is evidence of production vehicle or target hardware validation.

Both the backend and ECU query the kernel using rtnetlink `RTM_GETLINK` and
require `IFLA_LINKINFO/IFLA_INFO_KIND=vcan` before binding the CAN socket.
An interface named `vcan0` with another kind, including renamed physical CAN,
is rejected. Checking the name or ARPHRD_CAN alone would not establish this.

The independent `vehicle_ecu` process owns the simulated state. Its explicit
initial state is climate off and left front window closed. The backend starts
with unknown state, and requires a successful explicit query before a setter.
The ECU polls its descriptor and closes it on SIGTERM/SIGINT. Launchers and tests
must terminate only the process they created.

## CAN wire contract

Classical CAN, standard 11-bit IDs, DLC 8. Request ID `0x600`, response ID `0x601`.

| Byte | Request | Response |
| --- | --- | --- |
| 0 | version 1 | version 1 |
| 1 | query 1 / climate 2 / left front window 3 | ok 0 / invalid 1 / rejected 2 |
| 2-5 | transaction ID, big endian | matching transaction ID, big endian |
| 6 | setter value 0/1; query must be 0 | bit 0 climate; bit 1 left front window; other bits 0 |
| 7 | reserved 0 | reserved 0 |

The codec rejects EFF/RTR/error frames, wrong IDs/DLC/version/reserved fields,
unsupported codes and invalid value/state bits. The backend additionally rejects
responses with a stale transaction ID. Invalid frames supply no state and cannot
complete a request. The ECU discards malformed requests without changing state.

## Ownership and cancellation

Each backend serializes operations with one operation mutex and at most one
pending CAN transaction. A separate state mutex makes `snapshot()` available
while waiting. Callers must release their task-state mutex before invoking an
operation.

`SubmissionGate` is an optional caller-provided linearization boundary: it checks
current task/cancellation under the caller's mutex and either returns false or
executes the provided closure exactly once synchronously. The closure contains
only one nonblocking CAN write (or deterministic FakeVehicle state change).
The gate releases the caller's mutex before ACK polling. It must not store the
closure, invoke it twice, or return true without invoking it. Cancellation
predicates are called outside the backend state mutex.

Before submission, cancellation reports `action_applied=false`. After successful
write, cancellation waits for the matching ACK or the fixed 2-second deadline;
it never resends or rolls back an action. A successful setter ACK reports
`action_applied=true`, including an idempotent set. A successful query reports
false. Rejected setters report false and preserve previously confirmed state.
An OK setter receipt whose requested bit disagrees with the requested value is
an invalid response: state/action become unknown and require an explicit query.
A lost ACK or I/O error after write clears both state fields and action receipt
to unknown. Further setters refuse with `state_unknown` until an explicit
successful query restores the state. Snapshot optionals distinguish unknown
from explicit false; callers must preserve their presence in protobuf/QML.

## Reproduce

```sh
cmake -S agent/vehicle -B /home/projects/build/cabinflow-vehicle-can -DBUILD_TESTING=ON
cmake --build /home/projects/build/cabinflow-vehicle-can -j2
ctest --test-dir /home/projects/build/cabinflow-vehicle-can --output-on-failure
/home/projects/build/cabinflow-vehicle-can/vehicle_can_test --integration \
  /home/projects/build/cabinflow-vehicle-can/vehicle-ecu/vehicle_ecu
```

The unit target covers exact codec bytes, invalid frames, deterministic state,
gate refusal, pre-submit cancellation, initial unknown state, and unavailable
interface failure when `vcan0` is absent. The explicit integration command covers
owned ECU query/set/termination, stale/malformed response handling, setter
rejection, cancellation after write, readable snapshot during ACK wait, lost ACK,
and explicit query recovery. It exits nonzero when `vcan0` is absent; this is
pending integration, never a successful skipped bus test. This implementation
does not modify WSL kernels, configure network interfaces, or invoke sudo.

Before the live command, read-only prerequisite checks are `ip -details link show
vcan0` (the interface must exist, be UP and have kind vcan), `uname -r`, and
`test -d /sys/module/vcan`. An absent interface means an explicitly pending
integration result. Creating it requires an environment owner to provide a
kernel with CAN_RAW/vcan support and configure the virtual interface; that
environment change is outside this implementation. Even a successful live test
would prove only x86 Linux/WSL vcan simulation.
