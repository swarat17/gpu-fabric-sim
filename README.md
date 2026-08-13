# GPU Fabric Congestion Simulator & Router

A packet-level discrete-event simulator for GPU training fabrics, built to answer
one question rigorously: **how much does congestion-aware routing actually beat
ECMP on a collective workload, and where does it lose?**

> **Status: milestone M2 of 8.** The simulation core, k-ary fat-tree, ECMP with a
> tested 5-tuple hash, a fixed-window ack-clocked transport and the ring
> all-reduce workload are in place, validated against closed forms. The adaptive
> router lands in M3 and the headline comparison in M4. This section is replaced
> by the result when there is one.

## What works today

A 128-GPU ring all-reduce across a fat-tree, routed by ECMP:

```console
$ ./build/release/fabricsim --scenario=allreduce --k=8 --queue=2048
topology     : fat-tree k=8, 128 hosts, 80 switches, 768 directed links
collective   : ring all-reduce, 128 ranks, 254 steps, 1920000 B/GPU
chunk        : 15000 bytes (10 packets), 6 hops per step
placement    : round-robin over pods
routing      : ecmp (seed 0)
transport    : window 32 pkts, ack 64 B, rto 2953020 ns

collective   : 1277650 ns
analytical   : 609600 ns  (lower bound)
over ideal   : +109.59 %

data bytes   : injected 487680000, delivered 487680000, dropped 0
ack bytes    : injected 20807680, delivered 20807680, dropped 0
retransmits  : 0 packets
flows        : 32512 complete, 0 incomplete, 0 gave up
events       : 8453120 processed in 1.1021 s wall (7.67 M events/s)
digest       : 0x1fbd02214f1742b8
```

**That +110% is the gap M3 exists to close.** Nothing was dropped and nothing was
retransmitted, so none of it is loss recovery — it is queueing, and the queueing
is there because ECMP hashes two flows onto one uplink while a parallel uplink
sits idle. A ring all-reduce is a barrier: the step is not done until the slowest
rank's chunk lands, so one unlucky hash costs the whole collective.

and the single-flow scenario that anchors all of it to a closed form:

```console
$ ./build/release/fabricsim --scenario=smoke
topology     : 2 spine / 2 leaf / 8 hosts, 24 directed links
flow         : 1500000 bytes, mtu 1500, 1000 packets, 4 hops

fct simulated: 120760 ns
fct analytic : 120760 ns
delta        : 0 ns

bytes        : injected 1500000, delivered 1500000, dropped 0
flows        : 1 complete, 0 incomplete
events       : 9000 processed in 0.0005 s wall (17.70 M events/s)
```

The simulated flow completion time equals the closed form **exactly** — not
within a tolerance. That is the correctness anchor everything later rests on;
see [docs/validation.md](docs/validation.md) for the derivation and coverage.

### What the milestones so far actually showed

**Equal-cost paths are worth using.** At 25% offered load a 128-flow permutation
delivers every byte under ECMP, while single-path routing loses 63% of them —
every cross-pod flow funnelled through one aggregation and one core switch.

| k=8, 25% load | ECMP | single-path |
|---|---|---|
| bytes delivered | 128 000 000 (100%) | 47 128 500 (37%) |
| flows completed | 128 / 128 | 26 / 128 |

**An uncontrolled sender is a different problem from routing, and M2 fixed it.**
In M1 the source paced open loop, so a permutation at full line rate lost ~45% of
its bytes: two flows hashing onto one uplink is a sustained 2:1 overload and
nothing told the sender to slow down. With the ack-clocked window the same run
delivers every byte, retransmits nothing, and completes all 128 flows — the
congestion turns into queueing delay instead of loss, which is the thing worth
measuring.

**Ack overhead is charged, not waved away.** Acks are real packets on real links:
they queue, they can be dropped, and they take the reverse path chosen by the
reversed 5-tuple. On the collective above they add 20.8 MB of traffic to 487 MB
of data.

## How it works

- **Discrete-event, virtual time, no sockets.** Deliberate: the routing
  comparison is only meaningful if both arms saw identical inputs, and only
  reproducible if the same configuration yields the same answer on someone
  else's machine. [Why not real UDP](docs/design.md#why-a-discrete-event-simulator-and-not-real-sockets).
- **Packet-level, store-and-forward, drop-tail.** Queue depth and drops are
  exactly what an adaptive router consumes, so a fluid model would remove the
  thing being studied.
- **Fixed-window, ack-clocked transport.** Acks are real packets on real links,
  routed on the reversed 5-tuple, and a drop is recovered by a timer. No reactive
  congestion control — that is a stated limitation, not an oversight.
- **Routing policy is a template parameter, not a virtual call.** A choice made
  once per run has no business being re-resolved once per packet per hop. There
  is exactly one runtime `switch`, at the CLI boundary, where the flag becomes a
  type.
- **Handles, not pointers.** Every entity is a dense `uint32` index into a
  contiguous vector. `std::shared_ptr` appears nowhere in this project.
- **A directed link *is* a port.** Congestion is directional, so there is no
  separate `Link` entity — one less indirection per hop.
- **Integer-only core.** `uint64` nanoseconds and bytes; no floating point below
  `harness/`. Release, Debug and ASan builds all produce identical modelled
  output.
- **Single-threaded by construction.** A discrete-event simulator is sequential
  by definition, and threads would destroy the determinism everything depends
  on. Parallelism belongs at the scenario-sweep level. This is why there is no
  ThreadSanitizer job in CI.

## Validation

| check | result |
|---|---|
| Single-flow FCT vs closed form | exact equality, across 4 flow sizes × 3 delays × 4 link rates × 2 path lengths |
| Multi-flow FCT vs closed form | no flow ever beats its uncongested lower bound; at low load the fastest flow matches it exactly |
| All-reduce vs closed form | exact equality on a link-disjoint ring; lower bound on the cross-pod ring |
| Barrier semantics | every step starts exactly when the step it depends on finished, not one ns earlier |
| Window sizing | a one-packet window costs exactly one round trip per packet — asserted, not assumed |
| Loss recovery | a run with deliberately tiny buffers drops, retransmits, and still delivers every unique byte |
| ECMP hash uniformity | chi-square at p=0.999 over 4032 structured flows × {2,4,8,16} paths × 32 switch salts |
| Hash polarization | two switches pick the same path ≈1 time in 8, not always |
| Byte conservation | injected = delivered + dropped |
| Determinism | same digest across repeat runs, rebuilds, and Release/Debug/ASan |
| Paired-comparison precondition | for a given seed, both routing arms get byte-identical flow specs |
| Fat-tree shape | k³/4 hosts, 5k²/4 switches, k ports per switch, k²/4 cross-pod paths, at k = 2, 4, 6, 8 |
| Routing loop freedom | every equal-cost candidate is a strict step toward the destination |
| Port pairing / contiguity | asserted for every port in every topology under test |

70 test cases, 59 712 assertions, clean under `-Wall -Wextra -Wconversion
-Werror` and under ASan+UBSan. Release, Debug and ASan builds produce identical
digests for every scenario.

The percentile definition is nearest rank, and flows that never completed are
excluded from it and counted separately — see
[docs/validation.md](docs/validation.md#percentiles-nearest-rank) for why that is
the most dangerous number in the project.

## Build

Requires **GCC 13+** (or Clang 17+) for C++20 `std::format` and complete
`<ranges>`, plus CMake 3.21 and Ninja. On Ubuntu 22.04, which ships GCC 11:

```bash
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update && sudo apt-get install -y g++-13 cmake ninja-build valgrind

CXX=g++-13 cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `release`, `debug`, `asan` (RelWithDebInfo + ASan/UBSan).

If a local GCC 13 is inconvenient, the same build runs in a container:

```bash
docker run --rm -v "$PWD:/src" -w /src gcc:13 bash -c \
  "apt-get update && apt-get install -y cmake ninja-build && \
   cmake --preset release && cmake --build --preset release && ctest --preset release"
```

## Dependencies

One, vendored: [doctest](https://github.com/doctest/doctest) 2.4.11 as a single
header in `third_party/`, not fetched, so the build works with no network
access. Everything else — CLI parsing, formatting, the test harness wiring — is
in the standard library. `std::format` is why there is no `fmt` dependency.

The plotting layer (M4) will add numpy/pandas/matplotlib, isolated in `tools/`
and never linked into the C++ build.

## Layout

```
src/fabric/core/       virtual clock, event queue, simulation loop   [hot]
src/fabric/model/      links, ports, drop-tail queues                [hot]
src/fabric/routing/    multipath table, 5-tuple hash, policies       [hot]
src/fabric/topology/   k-ary fat-tree and leaf-spine generators
src/fabric/workload/   permutation, ring all-reduce, seeded RNG streams
src/fabric/metrics/    FCT percentiles
src/fabric/harness/    scenarios, closed forms, routing dispatch
apps/fabricsim/        CLI
tests/unit/            invariants, hash uniformity, percentile definition
tests/integration/     analytical validation, determinism, transport, collectives
docs/                  design.md, validation.md
```

## Documentation

- [docs/design.md](docs/design.md) — architecture, the discrete-event decision
  and what it costs, ownership model, hot path, determinism hazards.
- [docs/validation.md](docs/validation.md) — closed-form derivations, coverage,
  and how rounding artefacts are prevented from masquerading as results.
