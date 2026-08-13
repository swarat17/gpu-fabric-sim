# Validation

The simulator is checked against closed-form results, not against itself.

Because the core is integer-only (see [design.md](design.md)), these are exact
equalities rather than tolerances. A one-nanosecond error in the link model, the
store-and-forward pipeline or the event ordering fails the test.

## Single flow, uncongested, homogeneous links

**Setup.** One flow, source-paced at NIC line rate, crossing `h` identical links
of rate `R` and propagation delay `d`. Packet size `M`, flow size `S`, so
`n = S/M` packets.

**Derivation.** Let `ser = M·8/R` be the per-link serialisation time. The source
releases packet `i` at `i·ser`. Because the release period equals `ser`, each
port frees exactly as the next packet arrives: the pipeline never stalls and no
queue ever holds more than one packet. Packet `i` therefore lands at
`(i + h)·ser + h·d`, and the flow finishes with its last packet, `i = n−1`:

```
FCT = (n − 1 + h) · ser + h · d
```

Implemented as `analytical_fct_ns()` in `src/fabric/harness/scenario.hpp`;
asserted in `tests/integration/test_analytical_fct.cpp`.

**Worked example** — the `smoke` scenario, and the current output of
`fabricsim --scenario=smoke`:

| quantity | value |
|---|---|
| topology | 2 spine / 2 leaf / 8 hosts, 24 directed links |
| link rate `R` | 100 Gb/s |
| propagation delay `d` | 100 ns |
| MTU `M` | 1500 B |
| flow size `S` | 1 500 000 B → `n` = 1000 packets |
| hops `h` | 4 (host → leaf → spine → leaf → host) |
| `ser` = 1500·8/100e9 | 120 ns |
| **analytical** = (1000−1+4)·120 + 4·100 | **120 760 ns** |
| **simulated** | **120 760 ns** |
| delta | **0 ns** |

**Coverage.** The same equality is asserted across flow sizes (1 to 1000
packets), propagation delays (0, 100, 5000 ns), link rates (10, 25, 100,
400 Gb/s) and both the 4-hop cross-leaf and 2-hop same-leaf paths.

## Multi-flow: the lower-bound test

The closed form generalises to a source paced below line rate. With release
period `T` (equal to `ser` at full rate):

```
FCT = (n − 1) · T + h · ser + h · d
```

Implemented as `paced_fct_ns()`. Under *any* routing policy and *any* amount of
cross traffic this is a **lower bound** on a flow's completion time — congestion
can only add queueing delay, never remove it.

`tests/integration/test_permutation.cpp` asserts that no flow in a 16-flow
permutation ever beats its own bound. This is the cheapest strong check on the
whole model: a packet that skipped a hop, a link that forgot to charge
serialisation, or a routing table that took a short cut would all show up here.

A second test closes the other side. At 25% offered load nothing queues anywhere,
so the fastest flow must **equal** its bound exactly — not merely exceed it —
which catches a model that charges a delay it should not.

## Percentiles: nearest rank

The definition is part of the claim, so it is pinned by
`tests/unit/test_metrics.cpp` rather than left to whichever convention a plotting
library defaults to.

**p_q is the observation at rank `ceil(q · n)` of the ascending sample.** No
interpolation between neighbours, so every reported percentile is the completion
time of a flow that actually existed. p99 of 10 samples is the largest of them,
and that is intentional.

Flows that never completed are **excluded from the percentiles and counted
separately**. This is the most dangerous number in the project: percentiles over
the survivors flatter every one of them, because the flows that failed are
exactly the ones that were doing worst. `FctSummary::complete()` is false
whenever any flow is missing, and both the CLI and the tests are required to say
so rather than print a number that looks fine.

## The ECMP hash is tested, not assumed

A weak hash collides flows onto the same path more often than chance. That would
make ECMP look terrible, make the adaptive router look brilliant, and turn the
headline number into a measurement of the strawman. `tests/unit/test_ecmp_hash.cpp`
therefore asserts:

| check | why |
|---|---|
| Chi-square uniformity at p=0.999 over 4032 structured flows, for 2/4/8/16 candidates | sequential addresses and ports are where a weak mixer fails |
| The same, for 32 different switch salts | a hash uniform at one switch and lumpy at another gives the fabric permanently unlucky nodes |
| Two switches agree on a path ≈1 time in 8, not always | hash polarization across tiers |
| The choice is stable for a given flow | per-flow, not per-packet: this is why ECMP never reorders |
| Every 5-tuple field changes the hash | hashing addresses only is a real failure mode |

## Guarding against rounding artefacts

`serialization_ns()` truncates. A configuration where `M·8·10⁹` is not divisible
by `R` would make the simulator disagree with the closed form for a reason that
has nothing to do with the network model.

Rather than choose a tolerance and hope, `serialization_is_exact()` reports
whether a given (bytes, rate) pair divides evenly, and every validation scenario
carries an `analytical_exact` flag. The tests `REQUIRE` it before comparing, so
an inexact configuration fails loudly instead of silently comparing against a
rounded number.

## Conservation

Every injected byte must be either delivered or dropped once the event queue
drains. Asserted at the end of the integration scenarios.

There is no retransmission yet, so a dropped packet leaves its flow permanently
incomplete. `RunStats` counts incomplete flows explicitly rather than letting
them vanish from the statistics — a flow that never finishes must never be
allowed to quietly improve a completion-time percentile.

## Determinism

Three properties, all asserted in `tests/integration/test_determinism.cpp`:

1. Two independent builds of the same scenario produce the same result digest.
2. Re-running the same `Simulation` object reproduces the run exactly — which is
   also the state-reset test, since queues, port busy flags and counters must
   all be cleared.
3. The digest actually discriminates: a different configuration produces a
   different digest, so (1) and (2) are not vacuous.

Verified additionally across build configurations: Release, Debug and
RelWithDebInfo+ASan/UBSan all produce `fct = 120 760 ns` for the smoke scenario
and result digest `0x155eb497f134d2a8` for the 128-flow ECMP permutation. That is
the integer-only core doing its job — with floating point in the model,
optimisation level alone could move the last digits.

The permutation digest is the stronger of the two: it covers 128 flows, a hashed
routing decision at every hop, and roughly 700 000 events, any one of which
reordering would change the answer.

## Topology and routing invariants

From `tests/unit/`:

- **Port pairing** — every directed port's peer points back at it, with matching
  rate and delay. Without this, routing could send packets into a one-way street.
- **Contiguity** — a node's ports occupy one contiguous run, which is the entire
  reason `FabricBuilder` is two-phase.
- **Reachability** — every node has a route to every host.
- **Equal-cost breadth** — a leaf sees exactly one candidate per spine for a
  remote host. This is the property the ECMP and adaptive policies depend on.
- **Loop freedom** — every candidate at every node is a strict step toward the
  destination, so no routing policy, however it chooses among candidates, can be
  made to loop.

For the fat-tree (`tests/unit/test_fat_tree.cpp`), at k = 2, 4, 6, 8:

- **Shape** — k³/4 hosts and 5k²/4 switches; every switch has exactly k ports and
  every host exactly one. Building a fat-tree out of uniform k-port switches is
  its defining property, and a wiring bug shows up here before it shows up as a
  routing anomaly.
- **Path count** — cross-pod host pairs have exactly k²/4 equal-cost paths,
  same-pod pairs k/2, same-edge-switch pairs 1. Enumerated by walking the
  candidate sets, not asserted from the formula.
- **Tier breadth** — an edge switch leaving its pod sees k/2 uplinks; an
  aggregation switch sees k/2 cores; a core switch sees exactly one aggregation
  switch per pod. If any of these collapsed to a single candidate, ECMP and the
  adaptive router would be the same function.
- **Hop count** — following any candidate reaches the destination in exactly
  `fat_tree_hops()` hops, for all 240 ordered host pairs at k=4.
