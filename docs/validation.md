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
RelWithDebInfo+ASan/UBSan all produce `fct = 120 760 ns` for the smoke scenario.
That is the integer-only core doing its job — with floating point in the model,
optimisation level alone could move the last digits.

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
