# Design

## Why a discrete-event simulator, and not real sockets

The claim this project exists to support is a comparison between two routing
policies under congestion. That comparison is only worth reading if the two arms
saw identical inputs and if a reader can regenerate the numbers. Both properties
come from simulating virtual time.

The alternatives were considered and rejected:

**Real UDP between processes on one machine.** A laptop cannot host a 128-node
fat-tree at realistic link rates, so rates would have to be throttled
artificially — which is a simulator, plus kernel scheduler jitter, loopback
semantics and qdisc behaviour that are neither modelled nor controlled. The
congestion measured would be Linux's, not the fabric's, and bit-exact
reproducibility would be gone.

**A hybrid: discrete-event core with a real socket transport underneath.**
Reconciling virtual time with wall-clock time forces either time dilation or
loss of determinism. It is a second complete system for no added fidelity on
the routing question.

**An RDMA/verbs-style API over that transport.** Verbs — queue pairs, completion
queues, memory regions, doorbells — exists to move bytes to a NIC without
copies. Nothing in this project moves bytes: a packet is a 12-byte struct that
never leaves the process. Modelling memory regions for it would be ceremony.

What this costs: the project is not evidence of socket or kernel-bypass
programming. That is a real gap and it is not papered over here.

Because packet-level fidelity *is* required — queue depth and drops are exactly
what the adaptive router consumes, so a fluid model would remove the thing being
studied — the simulator has to process a lot of events, and the performance work
lands on the event loop rather than on a network stack. Simulator throughput is
reported as its own separate claim and is never mixed with the routing result.

## Ownership: handles, not pointers

Every entity is addressed by a dense `uint32` index into a contiguous vector
owned by `Simulation` (`src/fabric/core/ids.hpp`). This buys three things at
once:

- stable references across vector reallocation,
- cache-friendly layout on the hot path,
- determinism — nothing is ever ordered by a pointer value.

`NodeId`, `PortId` and `FlowId` are distinct types over the same underlying
`uint32` because mixing them is the likeliest silent bug in this codebase.

`std::shared_ptr` does not appear anywhere in this project. Ownership is a
single `Simulation` object; anything else is a handle or a `std::span`.

## A directed link *is* a port

There is no separate `Link` entity. Congestion is directional, so the queue, the
counters and (from M3) the telemetry all belong to one direction of one link.
Collapsing the two removes an indirection from the hop path, and the reverse
direction is reachable through `Port::peer_port` when it is needed. The
invariant that makes this a link at all — the two directed ports point at each
other — is asserted in `tests/unit/test_topology.cpp`.

## Integer-only core

Virtual time is `uint64` nanoseconds; sizes are `uint64` bytes. There is no
floating point below `harness/` and `metrics/`.

This is what makes "the same configuration produces the same answer" a claim
rather than a hope: floating-point contraction, x87 excess precision and
compiler reassociation would all quietly break bit-reproducibility across
compilers and optimisation levels. The one deliberate exception is
`core/scoped_timer.hpp`, which measures how long the simulator itself took —
host-machine instrumentation that is never fed back into the model.

Integer division in `serialization_ns()` truncates. Rather than pretend
otherwise, `serialization_is_exact()` reports whether a given (bytes, rate) pair
divides evenly, and the validation scenarios refuse to run when it does not —
so a rounding artefact can never be mistaken for a modelling result.

## The hot path

```
Simulation::run()
  └─ pop min Event                          [core/event_queue.hpp: binary heap]
     └─ switch on Event::type               [no virtual calls]
        ├─ Inject      → enqueue at NIC, schedule next release
        ├─ TxComplete  → hand to the wire, start next queued packet
        └─ Arrive      → deliver, or route and enqueue at an output port
```

Everything else — config parsing, topology construction, routing-table
construction, metrics, CSV output — is cold, may allocate, and is written for
clarity.

Three rules hold inside the loop: no allocation, no exceptions, no
`std::ranges`. Per-port queues and telemetry buffers are sized at
topology-construction time. `std::ranges` is confined to cold code because views
complicate profiles and can inhibit optimisation; there is no reason to pay that
in the one place it would be paid repeatedly.

## Event ordering and determinism

A binary heap imposes no order among equal keys. Two events scheduled for the
same nanosecond could otherwise come back in either order depending on heap
shape, and every published number would stop being reproducible.
`EventQueue::push` therefore stamps a monotonically increasing sequence number
and the comparison is `(time_ns, seq)` — a total order.

Ties are not hypothetical here. In a perfectly pipelined store-and-forward path
a packet arrives at a port at exactly the nanosecond that port finishes its
previous packet.

Other determinism hazards, all currently avoided and worth re-checking when code
is added: no `unordered_map` iteration in the core, no ordering by pointer
value, no floating point, and a separate RNG stream per concern once randomness
arrives in M1.

## Routing policy dispatch

`RouteTable::candidates()` returns a `std::span<const PortId>` of *every* output
port on a minimum-hop path — the equal-cost set, not a single pre-chosen next
hop. Three policies read the same table:

- **M0**: take element 0. Deliberately trivial.
- **M1**: ECMP, hashing the flow key across the span.
- **M3**: adaptive, choosing by queue occupancy at flowlet boundaries.

Building the table with multipath from the start is why none of that is a
retrofit. The property that makes any choice safe — every candidate is a strict
step toward the destination, so no policy can be made to loop — is asserted in
`tests/unit/test_routing.cpp`.

The policy is a compile-time template parameter on the hot path (no virtual call
per hop), with a single runtime-dispatch wrapper at the harness boundary for CLI
selection. That lands with ECMP in M1.

## Threading: there is none

A discrete-event simulator is sequential by definition — a single global event
order *is* the semantics. Parallel DES (conservative or optimistic
synchronisation, Time Warp) is a research problem, and it would destroy the
determinism that everything else here depends on.

Parallelism belongs at the scenario-sweep level instead: independent seeds are
independent processes, which needs no shared state and no lock-free anything.

This is why CI has no ThreadSanitizer job. The absence is a design consequence,
not an oversight.

## What is deliberately not here yet

- **Reactive congestion control** (DCQCN, TIMELY). Out of scope for the project;
  the transport model in M2 is a fixed ack-clocked window and ships as a stated
  limitation.
- **Retransmission.** M0 has none, so a dropped packet leaves its flow
  permanently incomplete — and `RunStats` reports incomplete flows rather than
  quietly dropping them from the statistics. Retransmission arrives with the
  transport model.
- **A custom allocator.** The handle-based design with pre-sized vectors may
  make one unnecessary. That is a question for M5 profiling to answer, and if
  the answer is "not needed", that is the result that gets reported.
