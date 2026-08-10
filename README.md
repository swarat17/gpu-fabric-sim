# GPU Fabric Congestion Simulator & Router

A packet-level discrete-event simulator for GPU training fabrics, built to answer
one question rigorously: **how much does congestion-aware routing actually beat
ECMP on a collective workload, and where does it lose?**

> **Status: milestone M0 of 8.** The simulation core, link model, topology,
> shortest-path multipath routing and the validation harness are in place and
> tested. ECMP, the all-reduce workload and the adaptive router land in M1–M3;
> the headline comparison lands in M4. This section is replaced by the result
> when there is one.

## What works today

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

## How it works

- **Discrete-event, virtual time, no sockets.** Deliberate: the routing
  comparison is only meaningful if both arms saw identical inputs, and only
  reproducible if the same configuration yields the same answer on someone
  else's machine. [Why not real UDP](docs/design.md#why-a-discrete-event-simulator-and-not-real-sockets).
- **Packet-level, store-and-forward, drop-tail.** Queue depth and drops are
  exactly what an adaptive router consumes, so a fluid model would remove the
  thing being studied.
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
| Byte conservation | injected = delivered + dropped |
| Determinism | same digest across repeat runs, rebuilds, and Release/Debug/ASan |
| Routing loop freedom | every equal-cost candidate is a strict step toward the destination |
| Port pairing / contiguity | asserted for every port in every topology under test |

26 test cases, 4324 assertions, clean under `-Wall -Wextra -Wconversion -Werror`
and under ASan+UBSan.

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
src/fabric/routing/    shortest-path multipath table                 [hot]
src/fabric/topology/   leaf-spine generator
src/fabric/harness/    scenarios and the analytical closed form
apps/fabricsim/        CLI
tests/unit/            invariants
tests/integration/     analytical validation, determinism
docs/                  design.md, validation.md
```

## Documentation

- [docs/design.md](docs/design.md) — architecture, the discrete-event decision
  and what it costs, ownership model, hot path, determinism hazards.
- [docs/validation.md](docs/validation.md) — closed-form derivations, coverage,
  and how rounding artefacts are prevented from masquerading as results.
