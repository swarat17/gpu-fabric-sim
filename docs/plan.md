# GPU Fabric Congestion Simulator & Router — Implementation Plan

## Context

Portfolio project to demonstrate systems-level modern C++ for networking/infra roles
(xAI Colossus networking, HFT-adjacent infra, distributed systems). The author has strong
Python/ML and ROS2 C++ but has not written performance-critical C++ at scale — the plan is
built around closing that gap visibly and honestly.

The deliverable is a **narrow, rigorous, reproducible claim**, not a broad demo:

> Flowlet-granular congestion-aware routing cuts p99 flow completion time by X%
> (95% CI: a–b) versus ECMP on a 128-GPU ring all-reduce sharing a k=8 fat-tree
> with a competing tenant.

Plus a second, deliberately separate claim about the simulator's own throughput. The two are
never conflated: routing quality is a modeling result; events/sec is a C++ result.

Greenfield — `G:\PROJECTS\Networks` is empty. Development on WSL2 Ubuntu 22.04.

---

## 0. Architecture decision (settled)

**Option A: pure discrete-event simulator, virtual time, no sockets. The RDMA/verbs layer is cut.**

**Why not B (real UDP).** A laptop cannot host a 128-node fat-tree at realistic link rates, so
rates get throttled artificially — that is a simulator, plus kernel scheduler jitter, loopback
semantics, and qdisc behavior that are neither modeled nor controlled. The congestion measured
would be Linux's, not the fabric's. Bit-exact reproducibility is lost, which is this project's
single most valuable property.

**Why not C (hybrid).** Reconciling virtual time with wall-clock forces either time dilation
(slow) or loss of determinism. It is a second complete system for zero added fidelity on the
routing claim — the six-weeks-of-infrastructure trap.

**Why the verbs abstraction is cargo cult here.** Verbs (QPs, CQs, MRs, doorbells) exists to move
bytes to a NIC without copies. Nothing moves bytes here; a "packet" is a ~32-byte struct that
never leaves the process. Memory regions for that read as resume padding. **Cut.**

**What is lost:** no evidence of socket/kernel-bypass programming. Do **not** patch this by
bolting sockets onto this project — it would weaken the fabric result without producing a
convincing HFT artifact. If that signal is wanted later, it belongs in a separate ~1-week repo
(busy-poll UDP echo + SPSC ring + latency histogram).

**Why the C++ story survives A.** Packet-level fidelity is *required* — queue depth and drops
are exactly what the adaptive router consumes, so a fluid model would gut the premise. Packet-level
events at fabric scale are genuinely expensive. The hot path becomes the event loop, so the
performance work is load-bearing rather than decorative.

### Three gaps in the original sketch (added to scope)

1. **Transport model.** Fixed-window, ack-clocked: a flow may have at most W bytes in flight.
   Without it, senders blast at line rate and FCT degenerates into a drop-and-retry artifact.
   Shipped with the stated limitation: *static window, no reactive congestion control.*
2. **Reordering cost.** Re-routing mid-flow reorders packets, which real RoCE NICs punish badly.
   An adaptive router that reorders for free is cheating. Solution: **flowlet switching** — only
   change path at an inter-packet gap exceeding the maximum path-delay delta, which makes
   reordering structurally impossible. This is what CONGA/Presto actually do.
3. **Telemetry staleness.** A router reading global instantaneous queue state is an oracle, not a
   router. Telemetry is **local to the switch** and **age-delayed**, with the delay swept.

---

## 1. Architecture

### Modules

| Module | Responsibility | Path | Hot? |
|---|---|---|---|
| `core` | Virtual clock, event queue, POD event types, IDs | `src/fabric/core/` | **Hot** |
| `topology` | k-ary fat-tree generation, link/switch tables | `src/fabric/topology/` | Lookup-hot, build-cold |
| `model` | Link serialization + propagation, output-port queues, drop-tail | `src/fabric/model/` | **Hot** |
| `routing` | `RoutingPolicy` concept; ECMP and adaptive implementations | `src/fabric/routing/` | **Hot** |
| `transport` | Fixed-window sender, ack clocking, retransmit-on-drop | `src/fabric/transport/` | Warm |
| `workload` | Ring all-reduce, background/incast tenant, permutation | `src/fabric/workload/` | Cold (emits events) |
| `telemetry` | Per-link counters, aged sampling into preallocated ring | `src/fabric/telemetry/` | Warm |
| `metrics` | FCT recording, percentiles, conservation checks | `src/fabric/metrics/` | Cold |
| `harness` | Config parse, scenario run, CSV emit | `src/fabric/harness/` | Cold |
| `tools/plot` | Python plotting | `tools/plot/` | n/a |

### Ownership model

- One `Simulation` object owns everything. Move-only, non-copyable, rule of zero.
- **Handles, not pointers.** `SwitchId`, `LinkId`, `PortId`, `FlowId`, `HostId` are distinct
  strong typedefs over `uint32_t`, indexing into contiguous `std::vector`s owned by `Simulation`.
  This gives stable references across reallocation, cache-friendly layout, trivial serialization,
  and determinism (no pointer-value-dependent ordering).
- **`std::shared_ptr` is banned project-wide.** State this in the README with the reason.
- No allocation on the hot path. All per-port queues and telemetry buffers are sized at
  topology-construction time.

### Key data structures

- **Event queue** — binary heap over POD `Event` in a `std::vector`. `Event` is ≤ 32 bytes:
  `uint64_t time_ns; uint64_t seq; uint8_t type; ...packed ids`. The `seq` field gives a *total*
  order under equal timestamps, which is what makes runs bit-identical. This is the single hottest
  structure and the natural target for M5 (timing wheel / calendar queue A/B, since link delays
  give bounded lookahead).
- **Output port queue** — fixed-capacity ring buffer, drop-tail, no allocation, no branch-heavy
  bookkeeping.
- **Routing tables** — flat arrays indexed by `(switch, dst_pod_or_host)` yielding a
  `std::span<const PortId>` of ECMP-valid candidates.
- **Integer-only core.** Time in `uint64_t` nanoseconds, sizes in `uint64_t` bytes. **No floating
  point anywhere in the core** — floats appear only in reporting. This buys bit-identical results
  across compilers and optimization levels, which is a claim worth making.

### Hot path (name it explicitly in `docs/design.md`)

```
Simulation::run()
  └─ pop min Event                         [core: heap]
     └─ dispatch on Event::type            [switch, no virtuals]
        ├─ PacketArrival → routing decision  [routing: span of candidates + policy]
        ├─ enqueue at output port            [model: ring buffer, drop-tail]
        ├─ compute serialization + prop      [model: integer math]
        └─ push next Event                   [core: heap]
```

Everything else — config, topology build, workload generation, metrics, CSV — is cold and may
allocate, use `std::ranges`, throw, and be written for clarity.

### Routing dispatch

`RoutingPolicy` is a **compile-time template policy** on the hot path (no virtual call per hop),
with a single runtime-dispatch wrapper at the harness boundary for CLI selection. This is the
natural design, not a performance affectation — and it is the honest place for a concept.

---

## 2. Modern C++ — accepted, with reasons

| Feature | Where | Why it is the natural choice |
|---|---|---|
| Handles + contiguous storage | `core/ids.hpp`, `core/simulation.hpp` | Cache locality, stable IDs, determinism. The foundational decision. |
| `std::span` | `routing/policy.hpp` (candidate ports), `telemetry/view.hpp` | Non-owning view over the flat routing table; zero copy, no template bloat at call sites. |
| Move semantics / rule of zero | `Simulation`, `Scenario`, result buffers | Ownership is unambiguous; no copy of multi-MB state. |
| Strong typedefs | `core/ids.hpp` | `LinkId` vs `PortId` mixups are the likeliest silent bug in this codebase. |
| `constexpr` | `topology/fattree.hpp` | Fat-tree pod/port index arithmetic genuinely is compile-time; enables `static_assert` on topology invariants (`k` even, host count `= k³/4`). |
| One `concept` | `routing/concepts.hpp` | `RoutingPolicy` replaces a comment with a compiler-checked contract and produces readable errors. **One** concept, not ten. |
| `std::ranges` | `metrics/`, `harness/` only | Cold-path clarity. **Explicitly banned on the hot path** — views complicate profiling and can inhibit optimization. |
| RAII | `core/scoped_timer.hpp`, CSV writers | Scoped profiling timers and file handles; the obvious use. |
| Cache-line awareness | `core/event.hpp` (≤32B packing), `model/port.hpp` | Measured, not assumed. `alignas(64)` only where a profile shows it matters. |

### Rejected as feature tourism — and the reason

- **Lock-free / SPSC queues, threads in the simulator.** A DES is inherently sequential; a global
  event order is the semantics. Parallel DES (conservative/optimistic, Time Warp) is a research
  problem that would consume the schedule *and destroy determinism* — the project's most valuable
  property. Parallelism belongs at the scenario-sweep level, where `xargs -P` over independent
  seeds needs no lock-free anything. **Cut.**
- **A telemetry writer thread with an SPSC ring.** Defensible in principle, but it introduces
  nondeterministic output ordering for no benefit. Buffer telemetry in memory, write at run end. **Cut.**
- **Custom arena/bump allocator.** **Deferred, not scheduled.** The handle-based design with
  pre-sized vectors may make it unnecessary. If M5 profiling shows allocation on the hot path,
  add `core/arena.hpp` and report the before/after. If it does not, write *"I profiled; allocation
  was not the bottleneck; no custom allocator was needed"* in the README — that is a stronger
  signal than shipping an unused allocator.
- **Coroutines** ("each flow is a coroutine"). Tempting and superficially elegant, but frame
  allocation and obscured control flow are exactly the wrong trade on a hot path, and it is a trap
  for someone new to performance C++. **Cut.**
- **PMR.** Superseded by the handle design. **Cut.**
- **Exceptions on the hot path.** None. Validation and error reporting happen at config-parse and
  topology-build boundaries.

---

## 3. Milestones

Each ends in something runnable and measurable. Thin end-to-end slice in week 1.
Hours are calibrated for someone fluent in C++ syntax but new to performance work.

| # | Deliverable | Runnable proof | Hours |
|---|---|---|---|
| **M0** | Hello fabric: hardcoded 4-switch/8-host topology, one flow, packet-level, static routing. CMake + presets + 1 test + CI skeleton. | `./fabricsim --scenario smoke` prints a flow completion time | 8–12 |
| **M1** | k-ary fat-tree generator, ECMP (5-tuple hash), multi-flow, link model + drop-tail queues, FCT percentiles | Permutation traffic on k=4 and k=8; prints p50/p99 | 12–16 |
| **M2** | Ring all-reduce workload, fixed-window transport, **analytical validation suite** | Simulated all-reduce time matches `2(N−1)α + 2(N−1)/N · S/B` within tolerance on an uncongested fabric | 10–14 |
| **M3** | Local aged telemetry, flowlet-granular adaptive router, competing-tenant workload, **first A/B number** | `./fabricsim --routing {ecmp,adaptive}` on the headline scenario | 14–18 |
| **M4** | Measurement rigor: paired seeds, hold-out seeds, bootstrap CIs, load sweep, one-command repro, plots | `./scripts/bench.sh` → CSVs + PNGs, ~8 min | 10–14 |
| **M5** | Simulator performance work: profile, fix top 2–3 hotspots, event-queue A/B (heap vs timing wheel), report events/sec and max fabric size | Before/after table + callgrind profile in README | 12–16 |
| **M6** | **Failure-mode study**: scenarios where adaptive *loses*, written up | `docs/failure_modes.md` + ratio plot crossing 1.0 | 8–12 |
| **M7** | README, diagrams, docs polish, committed results | Repo skims well in 90 seconds | 6–10 |
| | | **Total** | **80–112 h** |

At 15–20 h/week that is **5–6 weeks**. M2's analytical validation and M6's failure study are the
two things that most distinguish this from a typical portfolio repo — do not let them get squeezed.

### Cut from the original sketch

| Cut | Why it does not change the final claim |
|---|---|
| Real sockets / hybrid transport | Argued in §0 |
| RDMA/verbs API | Cargo cult for a DES |
| **Live** fabric heatmap / animation | 8–15 h of demo candy, zero effect on the claim. *Keep one static per-link utilization heatmap (~1 h) — it is genuinely illustrative.* |
| Tree / halving-doubling all-reduce | Ring alone supports the claim. Add only if M7 finishes early. |
| Multiple adaptive algorithms | One adaptive router + ECMP + one lower-bound reference. More variants dilute rather than strengthen. |
| DCQCN / reactive congestion control | A project in itself, and it entangles the routing result with CC tuning. Ships as a stated limitation. |
| Threading anywhere in the simulator | §2 |

---

## 4. Measurement methodology

This section is the credibility of the project. It gets its own `docs/methodology.md`.

**Determinism**
- `std::mt19937_64`, seeded from config. **Separate RNG streams per concern** (workload arrival,
  hash tiebreak, tenant traffic) so changing one does not perturb the others.
- CI test: same seed twice → identical hash of the FCT vector.
- Determinism hazards to audit: no `unordered_map` iteration in the core, no pointer-value
  comparisons, no floating point in the core (§1).

**Paired comparison**
- ECMP and adaptive run on **identical inputs** from the same seed. Report the *paired* difference
  distribution, not two independent means. This removes most cross-seed variance and is the
  statistically correct test.

**Statistics**
- N = 30 seeds minimum.
- FCT distributions are heavy-tailed, so use **bootstrap CIs over seeds**, not normal-theory intervals.
- Define the statistic precisely and state it in the README: *per-run p99 over all flows, then
  bootstrapped across the 30 per-run p99 values.* Ambiguity here is the most common quiet cheat.

**Hold-out seeds**
- Tune every adaptive hyperparameter (flowlet gap, telemetry age, queue-depth threshold) on
  **seeds 0–9**. Report exclusively on **seeds 100–129**, never touched during tuning.
  One sentence in the README; disproportionate credibility.

**Sweeps, not points**
- Report across offered load (30%–95%). A single operating point is the #1 cherry-picking smell.
- Sensitivity sweeps: buffer depth, telemetry age, flowlet gap, MTU, tenant intensity.

**Warmup, honestly**
- For the all-reduce collective, the workload *is* a transient — measure the full collective; do
  not apply a warmup discard. For background/permutation traffic, discard the first 10% of flows.
  Applying warmup uniformly would be wrong, and saying why demonstrates you understand it.

**Reference bounds**
- Report the analytical uncongested all-reduce time as a lower bound so a reader can see how much
  headroom actually exists.

### What would make these numbers NOT credible — guard against each explicitly

1. **A strawman ECMP.** A weak hash function causes pathological flow collisions and makes ECMP
   look terrible for free. Mitigation: use a real 64-bit mix over the 5-tuple, and ship a
   **chi-square uniformity unit test** on flow-to-path distribution.
2. **Tuning adaptive but not the baseline.** ECMP is parameterless, so the obligation is to prove
   it is a *good* ECMP — see (1).
3. **Tuning and reporting on the same seeds.** Handled by hold-out seeds.
4. **Free reordering.** Handled structurally by flowlet switching. Note in the README that with a
   large flowlet gap the router degenerates to ECMP — and show that limit in the sweep.
5. **Oracle telemetry.** Handled by local, age-delayed telemetry; the delay is a swept parameter.
6. **Uncharged control overhead.** Account for telemetry traffic and per-decision cost.
7. **Reporting only the winning scenario.** Handled by M6.

---

## 5. Build, test, CI, tooling

### Toolchain

- **GCC 13 minimum** (`std::format`, complete `<ranges>`, concepts). Ubuntu 22.04 ships GCC 11 —
  install via `ppa:ubuntu-toolchain-r/test`. Document this in the README build section.
- C++20. CMake ≥ 3.20 with `CMakePresets.json`: `debug`, `asan`, `release`.
- Targets: `fabric_core` (static lib), `fabricsim` (CLI), `fabric_tests`.

### Dependency policy — justify every one

| Dep | Verdict | Justification |
|---|---|---|
| **doctest** | Accept, **vendored** as `third_party/doctest.h` | Single header, fastest compile of the test frameworks. Vendored not FetchContent so the build works offline. Writing a test runner is not the point of this project. |
| numpy / pandas / matplotlib | Accept, `tools/requirements.txt` | Plotting in C++ would waste weeks for zero signal. Fully isolated from the C++ build. |
| fmt | **Reject** | `std::format` on GCC 13 covers it. Zero deps beats one dep. |
| Boost, spdlog, a JSON library | **Reject** | Nothing needed from them. Config is `key=value` text parsed by ~80 lines using `std::string_view` — itself a small clean-code exhibit. |
| Google Benchmark | **Reject** | The unit of measurement is a whole-program scenario run, not a microbenchmark. If the M5 event-queue A/B wants microbenchmarks, hand-roll it or vendor nanobench (single header). |
| cxxopts | **Reject** | CLI is `--key=value` overrides on the config file. Trivial. |

### Tests

- **Unit** — fat-tree invariants (host count `k³/4`, core count `(k/2)²`, all-pairs reachability);
  ECMP hash uniformity (chi-square); drop-tail semantics at exact capacity; percentile computation
  against a known vector; event-queue ordering under identical timestamps.
- **Analytical / golden** — single flow across an uncongested path: FCT must equal
  `size/B + hops·(prop_delay + serialization)` exactly (integer math makes this an equality, not a
  tolerance). Then ring all-reduce against its closed form. **These catch the large majority of
  modeling bugs and are the tests to feature in the README.**
- **Conservation** — bytes sent == bytes delivered + bytes dropped + bytes in flight, asserted at
  end of every integration scenario.
- **Determinism** — same seed twice → identical result hash.
- **Integration** — small scenarios under a wall-clock budget; snapshot the summary CSV.

### CI (GitHub Actions, `ubuntu-24.04`)

- Matrix: {gcc-13, clang-17} × {Release, ASan+UBSan}. Run full test suite.
- Run the fast benchmark preset and upload the summary CSV as an artifact — proves reproducibility
  to anyone reading the Actions tab.
- **No TSan** — the simulator is single-threaded by construction. Say so in `docs/design.md`
  rather than silently omitting it.

### Performance tooling (WSL2)

- **`valgrind --tool=callgrind` / `cachegrind`** as the primary profiler. WSL2 has no PMU
  virtualization so `perf stat` hardware counters are unavailable — but cachegrind's *simulated*
  cache model needs no PMU, and its determinism pairs perfectly with a deterministic simulator,
  giving reproducible cache-miss counts that a reader can regenerate exactly. Frame this as a
  deliberate methodological fit, not a workaround.
- **Wall-clock events/sec** via `core/scoped_timer.hpp`, reported per scenario.
- **heaptrack** if allocation turns out to matter.
- `kcachegrind`/`gprof2dot` output committed as a PNG in the README.

---

## 6. Failure modes — where the adaptive router loses to ECMP

Shipped as `docs/failure_modes.md` plus a bar plot of adaptive/ECMP p99 ratio across scenarios,
**with bars crossing 1.0**. Almost no portfolio repo does this; it is the strongest single
credibility signal available.

| # | Scenario | Mechanism |
|---|---|---|
| 1 | **Uniform traffic at low load** | Nothing to rebalance. Adaptive adds decision noise and telemetry lag and occasionally moves flowlets needlessly. ECMP ties or wins. |
| 2 | **Stale telemetry** | Router reacts to a queue that already drained. Many sources independently pick the same "least loaded" port, overload it, then all flee — **herding oscillation**. Expose by sweeping telemetry age upward; expect a clear crossover point. |
| 3 | **Clean isolated ring all-reduce** | Ring all-reduce is already a near-perfect permutation. Little imbalance exists to exploit, so the win is small or zero. **This is why the headline is all-reduce *with a competing tenant*** — and reporting the clean case honestly is what makes the headline believable. |
| 4 | **Short flows / small RPCs** | Flow completes before telemetry updates. Adaptive is pure overhead. |
| 5 | **Large flowlet gap** | Degenerates to ECMP by construction, plus decision cost. Shows up as the right-hand tail of the flowlet sweep. |
| 6 | **Heavily oversubscribed core** | The bottleneck is aggregate capacity; no routing decision fixes it. Both converge, and adaptive can lose slightly to queue churn. |
| 7 | **Adversarial permutation** | Constructed so every locally-greedy decision is globally wrong: local adaptive < ECMP < global optimum. Worth including precisely because it is the honest ceiling on local schemes. |
| 8 | *(counterpoint)* **Link failure / asymmetric topology** | Where adaptive wins decisively — ECMP keeps hashing flows into degraded paths. Include so the failure section reads as analysis, not self-flagellation. |

---

## 7. Repo structure and README

```
README.md
CMakeLists.txt  CMakePresets.json
docs/           design.md  validation.md  methodology.md  failure_modes.md
src/fabric/     core/ topology/ model/ routing/ transport/ workload/ telemetry/ metrics/ harness/
apps/fabricsim/ main.cpp
tests/          unit/  integration/
scenarios/      *.cfg          # headline, clean_allreduce, incast, link_failure, adversarial
scripts/        bench.sh       # the one command
tools/plot/     *.py  requirements.txt
third_party/    doctest.h
results/        *.csv  *.png   # COMMITTED
.github/workflows/ci.yml
```

**Commit `results/`.** A 90-second skimmer will not build anything; the plots must render on the
GitHub page.

### README, in skim order

1. **The claim with the number, above the fold**, one sentence, with the CI and the scenario named.
2. **The headline plot**, immediately — before any prose.
3. **`./scripts/bench.sh` — one command, ~8 min on a laptop, regenerates every number and plot.**
4. **"Where it loses"** — 3 bullets and a link to `failure_modes.md`. Put this *high*, not buried;
   it is the differentiator.
5. **How it works** — 6 bullets plus one topology/hot-path diagram.
6. **Validation** — table of simulated vs closed-form analytical results.
7. **Simulator performance** — events/sec, largest fabric simulated, before/after profile.
8. **Limitations, explicitly** — no reactive congestion control, no PFC, no real sockets,
   single-threaded, static window.
9. **Build** — GCC 13 requirement, three commands.

**No roadmap or TODO section.** A hiring engineer reads aspirational sections as unfinished work.

---

## Verification

End-to-end, in order:

1. `cmake --preset asan && cmake --build --preset asan && ctest --preset asan` — full suite clean
   under ASan+UBSan.
2. `./build/release/fabricsim --scenario scenarios/validation.cfg` — analytical checks pass as
   exact integer equalities.
3. Run the headline scenario twice with the same seed; assert identical result hashes.
4. `./scripts/bench.sh` from a clean checkout — produces every CSV and PNG in `results/`
   in under ~10 minutes, with no network access.
5. `valgrind --tool=callgrind ./build/release/fabricsim --scenario scenarios/headline.cfg`
   before and after M5; the instruction-count and cache-miss deltas go in the README.
6. Confirm the failure-mode plot actually contains bars above 1.0 — if adaptive wins everywhere,
   the scenario set is not adversarial enough and M6 is not done.
