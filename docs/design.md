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
        ├─ Inject      → send whatever the window and pacing allow
        ├─ TxComplete  → hand to the wire, start next queued packet
        ├─ Arrive      → route onward; or deliver and ack; or open the window
        └─ Timeout     → retransmit, unless acked or superseded
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
is added: no `unordered_map` iteration in the core, no ordering by pointer value,
and no floating point.

Randomness arrived in M1 and brought two more rules, both enforced by
`src/fabric/workload/rng.hpp`:

- **One stream per concern.** Workload placement, port numbers and (from M3)
  tenant traffic each draw from their own generator, seeded by name. Sharing one
  would mean that adding a single draw to the workload shifts every subsequent
  number and silently changes traffic elsewhere in the scenario — which would
  make a paired ECMP-vs-adaptive comparison compare two different experiments.
- **No `std::uniform_int_distribution`.** `mt19937_64` is specified bit-exactly
  by the standard; the distributions are not, and libstdc++ and libc++
  legitimately produce different values from the same engine. The bounded draw is
  implemented directly, by rejection.

`tests/integration/test_permutation.cpp` asserts the property that actually
matters downstream: for a given seed, the ECMP arm and the static arm are handed
byte-identical flow specifications.

## Topology: the k-ary fat-tree

`src/fabric/topology/fat_tree.hpp`. Every switch has exactly k ports; there are
k pods of k/2 aggregation and k/2 edge switches, (k/2)² cores, and k³/4 hosts.
Between two pods there are **k²/4 equal-cost paths** — 16 at k=8, the size every
published number uses.

That path count is the whole reason the topology is here. On a single-path
fabric ECMP and an adaptive router are the same function; the interesting
question only exists because there is a choice to get right or wrong.

The pod/port index arithmetic is `constexpr`, so the invariants (`k` even, hosts
= k³/4, switches = 5k²/4) are `static_assert`ed rather than tested at runtime.
The runtime tests cover the wiring the arithmetic cannot: every switch really
has k ports, every port pairs back, and the equal-cost breadth at each tier is
what the routing policies expect.

Routing is unchanged from M0 — BFS shortest paths, no fat-tree-specific logic.
On a Clos every minimum-hop path is a legitimate up-then-down path, so hop count
alone already produces exactly the right candidate sets. The table's memory does
grow as `node_count × host_count` cells, which is fine through k=8 (about 1.6 MB)
and would need a pod-level table before k=16.

## Routing policy dispatch

`RouteTable::candidates()` returns a `std::span<const PortId>` of *every* output
port on a minimum-hop path — the equal-cost set, not a single pre-chosen next
hop. Three policies read the same table:

- **`StaticFirstPolicy`**: take element 0. Not a strategy — it is the M0
  single-path behaviour, kept because the analytical validation needs a
  predetermined path to compare against a closed form.
- **`EcmpPolicy`** (M1): hash the 5-tuple across the span.
- **adaptive** (M3): choose by queue occupancy at flowlet boundaries.

Building the table with multipath from the start is why none of that is a
retrofit. The property that makes any choice safe — every candidate is a strict
step toward the destination, so no policy can be made to loop — is asserted in
`tests/unit/test_routing.cpp` and again for the fat-tree in
`tests/unit/test_fat_tree.cpp`.

The policy is a **compile-time template parameter** of `Simulation::run`
(`RoutingPolicy` concept, `src/fabric/routing/policy.hpp`), explicitly
instantiated once per shipped policy at the bottom of `simulation.cpp`. At one
decision per packet per hop, a choice made once per run has no business being
re-resolved a hundred million times. Exactly one runtime `switch` exists, in
`run_with_routing()` at the harness boundary, where the CLI string becomes a
type.

## The ECMP baseline has to be good

`src/fabric/routing/flow_key.hpp`. ECMP hashes the 5-tuple (src/dst address,
src/dst port, protocol) with a splitmix64 finalizer and indexes the candidate
set by multiply-shift rather than modulo — one multiply instead of a divide, with
bias bounded by n/2³² .

Two details matter more than the hash function itself:

- **Per-switch salt.** If every switch hashed identically, two flows that
  collided at the edge would collide again at the aggregation tier and again at
  the core — *hash polarization*, a real and well documented failure of naive
  multi-tier ECMP. One salt per switch decorrelates the tiers.
- **The salt comes from the run seed**, not a constant. A fixed hash seed can be
  accidentally lucky or unlucky on a given topology, and that luck would then be
  baked into every seed of every sweep. Varying it turns hash luck into ordinary
  run-to-run variance, which the paired comparison in M4 is built to absorb.

The uniformity of the result is a **test**, not an assumption:
`tests/unit/test_ecmp_hash.cpp` runs a chi-square test on flow-to-path
distribution over structured inputs at 2, 4, 8 and 16 candidates, checks it holds
for many switch salts rather than one lucky one, and checks that two switches
agree about 1 time in 8 rather than always. A weak hash would collide flows more
often than chance, ECMP would look terrible for free, and the headline number
would be measuring the strawman rather than the routing idea.

## The transport: fixed window, ack clocked

A sender keeps at most `window_pkts` packets outstanding and releases the next
one when an ack comes back. Below that, `FlowSpec::load_permille` paces sends as
a fraction of NIC line rate — a closed-loop sender alone has no notion of offered
load, and M4's sweep needs one.

Why this was needed before the adaptive router: in M1 the source was open loop,
so a permutation at full line rate lost about 45% of its bytes. Two flows hashing
onto one uplink is a sustained 2:1 overload, a 128-packet buffer cannot absorb
it, and nothing told the sender to slow down. Comparing routers under those
conditions would have measured which router drops less, through a mechanism no
real fabric would allow. With the window in place the same run delivers every
byte and the congestion shows up as queueing delay — which is the quantity the
adaptive router is supposed to reduce.

Three decisions inside it are worth defending:

- **Acks are real packets.** They are routed on the *reversed* 5-tuple, so the
  reverse path is hashed independently and need not mirror the forward path, as
  in a real fabric. They queue, they consume reverse-path capacity, and they can
  be dropped. Modelling them as free would make the feedback loop faster than any
  real one and would quietly correlate ack delivery with data delivery.
- **Loss is detected by a timer, and the timer is generous.**
  `default_rto_ns()` covers a full round trip plus a completely full buffer at
  every hop. A timeout that fires while a packet is merely queued produces a
  spurious retransmission — and does so *in proportion to how congested a path
  is*, which would systematically penalise whichever router leaves packets queued
  longest. That is exactly the quantity under study, so slow recovery from a real
  drop is the cheaper error.
- **Timers are invalidated lazily.** There is no cheap way to remove an entry
  from the middle of a binary heap, so a timer carries the attempt number it
  belongs to and a superseded one is recognised and dropped when it fires.

Deliberately absent: any reactive congestion control. No DCQCN, no TIMELY, no
window adaptation. That is a project in itself and it would entangle the routing
result with congestion-control tuning; it ships as a stated limitation.

Note that completion and failure are not opposites. Completion is a property of
the receiver — it has every byte. Failure is a property of the sender — it
exhausted `max_attempts` on some packet. A sender whose acks are being dropped
can give up on a packet the receiver already holds, so a flow can be both.

## Flow dependencies, and why a collective is not a batch of flows

`FlowSpec::depends_on` names a flow that must complete before this one starts;
`Simulation` keeps a CSR table of dependents and releases them on completion,
starting the dependent's completion-time clock at that moment rather than at its
nominal start time.

That one mechanism is all the simulator core knows about collectives. Ring
all-reduce is then just 2(N−1) steps of N flows with the chain wired up in
`workload/all_reduce.cpp`: rank r cannot send its step-s chunk until it has
received the step s−1 chunk from rank r−1.

The barrier is the point. A collective finishes when the slowest link in the
dependency chain finishes, not when the average one does, so a routing decision
that leaves one flow queued behind another does not cost the average — it costs
the entire collective. This is why the project reports p99 rather than mean FCT,
and why "a few flows got unlucky" is not a small problem.

A failed flow therefore strands everything downstream of it, which is visible and
intended: with deliberately tiny buffers, a k=4 collective reports 10 senders that
gave up and 73 flows that never completed.

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

- **Reactive congestion control** (DCQCN, TIMELY). Out of scope; the transport is
  a fixed ack-clocked window and ships as a stated limitation.
- **Ack coalescing.** Every data packet is acked individually. Real NICs coalesce;
  doing so here would reduce reverse-path load and slightly weaken the feedback
  loop, and neither effect is what the project is measuring.
- **Lossless fabric mechanisms** (PFC, credit flow control). The fabric is
  drop-tail, and losses are recovered by timeout. Deep buffers rather than link-
  level backpressure are how the headline configuration avoids loss.
- **A custom allocator.** The handle-based design with pre-sized vectors may
  make one unnecessary. That is a question for M5 profiling to answer, and if
  the answer is "not needed", that is the result that gets reported.
