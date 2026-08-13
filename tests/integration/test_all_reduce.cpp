#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "fabric/harness/scenario.hpp"
#include "fabric/metrics/fct.hpp"
#include "fabric/topology/fat_tree.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// Ring all-reduce: 2(N-1) dependent steps, one S/N chunk per step.
//
// The exact-identity test runs the ring inside a single edge switch, where the
// flows are link-disjoint by construction and nothing can queue anywhere. It
// also gives acks zero wire time. Both are deliberate: the identity is there to
// validate the *structure* of the collective -- the step count, the chunk
// arithmetic and the barrier -- not to pretend acks are free. Everything else in
// this file treats the closed form as a lower bound, which is what it is.
// ---------------------------------------------------------------------------

namespace {

// Ranks 0..3 all sit under edge switch 0 of pod 0 when k = 8, so every ring hop
// is host -> edge -> host and no two flows share a directed link.
AllReduceConfig disjoint_ring(Bytes ack_bytes) {
  AllReduceConfig cfg;
  cfg.topology.k = 8;
  cfg.routing = RoutingAlgorithm::Ecmp;
  cfg.workload.gpu_count = 4;
  cfg.workload.placement = RingPlacement::Sequential;
  cfg.workload.buffer_bytes = 4 * 20 * 1500;  // chunk = 20 packets
  cfg.workload.window_pkts = 32;
  cfg.transport.ack_bytes = ack_bytes;
  return cfg;
}

AllReduceConfig cross_pod_ring() {
  AllReduceConfig cfg;
  cfg.topology.k = 4;
  cfg.routing = RoutingAlgorithm::Ecmp;
  cfg.workload.placement = RingPlacement::RoundRobinPods;
  cfg.workload.buffer_bytes = 16 * 10 * 1500;  // 16 GPUs, chunk = 10 packets
  cfg.workload.window_pkts = 32;
  return cfg;
}

}  // namespace

TEST_CASE("the collective has 2(N-1) steps and each rank sends to its ring successor") {
  AllReduceScenario sc = build_all_reduce(disjoint_ring(1));
  const Fabric& f = sc.sim.fabric();
  const std::span<const FlowSpec> specs = sc.sim.specs();

  CHECK(sc.gpus == 4);
  CHECK(sc.steps == 6);
  CHECK(sc.chunk_packets == 20);
  REQUIRE(specs.size() == static_cast<std::size_t>(sc.steps) * sc.gpus);

  for (std::uint32_t s = 0; s < sc.steps; ++s) {
    for (std::uint32_t r = 0; r < sc.gpus; ++r) {
      const FlowSpec& spec = specs[s * sc.gpus + r];
      CAPTURE(s);
      CAPTURE(r);
      CHECK(f.host_index(spec.src) == r);
      CHECK(f.host_index(spec.dst) == (r + 1) % sc.gpus);

      if (s == 0) {
        CHECK_FALSE(spec.depends_on.valid());
      } else {
        // Rank r waits on the chunk rank r-1 sent in the previous step.
        const std::uint32_t upstream = (r + sc.gpus - 1) % sc.gpus;
        CHECK(spec.depends_on == FlowId{(s - 1) * sc.gpus + upstream});
      }
    }
  }
}

TEST_CASE("a link-disjoint ring matches the all-reduce closed form exactly") {
  AllReduceScenario sc = build_all_reduce(disjoint_ring(1));
  REQUIRE_MESSAGE(sc.analytical_exact,
                  "the configuration must make the closed form an identity, "
                  "otherwise this test compares against an optimistic number");
  REQUIRE(sc.uniform_hops);
  REQUIRE(sc.hops == kFatTreeSameEdgeHops);

  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);
  REQUIRE(stats.flows_incomplete == 0);
  REQUIRE(stats.bytes_dropped == 0);
  REQUIRE(stats.packets_retransmitted == 0);

  CHECK(collective_time_ns(sc.sim.results()) == sc.analytical_ns);
}

TEST_CASE("every step waits for the step before it") {
  // The barrier is the whole reason a collective is not just a batch of flows:
  // a rank cannot forward a chunk it has not received.
  AllReduceScenario sc = build_all_reduce(disjoint_ring(1));
  run_with_routing(sc.sim, sc.routing, sc.seed);

  const std::span<const FlowSpec> specs = sc.sim.specs();
  const std::span<const FlowResult> results = sc.sim.results();

  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (!specs[i].depends_on.valid()) {
      CHECK(results[i].start_ns == 0);
      continue;
    }
    const FlowResult& dep = results[specs[i].depends_on.index()];
    REQUIRE(dep.complete);
    CAPTURE(i);
    // Released exactly when its predecessor completed, and not one ns earlier.
    CHECK(results[i].start_ns == dep.finish_ns);
  }
}

TEST_CASE("real acks cost wire time, and the closed form becomes a lower bound") {
  AllReduceScenario ideal = build_all_reduce(disjoint_ring(1));
  AllReduceScenario real = build_all_reduce(disjoint_ring(64));

  REQUIRE(ideal.analytical_exact);
  // 64-byte acks occupy the sender's uplink alongside its data, so the harness
  // must stop claiming an identity.
  REQUIRE_FALSE(real.analytical_exact);
  REQUIRE(ideal.analytical_ns == real.analytical_ns);

  run_with_routing(ideal.sim, ideal.routing, ideal.seed);
  run_with_routing(real.sim, real.routing, real.seed);

  const Nanos t_ideal = collective_time_ns(ideal.sim.results());
  const Nanos t_real = collective_time_ns(real.sim.results());

  REQUIRE(t_ideal > 0);
  REQUIRE(t_real > 0);
  CHECK(t_ideal == real.analytical_ns);
  CHECK(t_real >= real.analytical_ns);
}

TEST_CASE("round-robin placement puts every ring hop across pods") {
  AllReduceScenario sc = build_all_reduce(cross_pod_ring());

  CHECK(sc.gpus == fat_tree_host_count(4));
  CHECK(sc.steps == 2 * (sc.gpus - 1));
  CHECK(sc.uniform_hops);
  CHECK(sc.hops == kFatTreeCrossPodHops);
}

TEST_CASE("a cross-pod collective never beats its uncongested lower bound") {
  AllReduceScenario sc = build_all_reduce(cross_pod_ring());
  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);

  REQUIRE(stats.flows_incomplete == 0);
  const Nanos measured = collective_time_ns(sc.sim.results());
  REQUIRE(measured > 0);
  CAPTURE(measured);
  CAPTURE(sc.analytical_ns);
  CHECK(measured >= sc.analytical_ns);
}

TEST_CASE("a collective run is reproducible") {
  AllReduceScenario a = build_all_reduce(cross_pod_ring());
  AllReduceScenario b = build_all_reduce(cross_pod_ring());

  const RunStats sa = run_with_routing(a.sim, a.routing, a.seed);
  const RunStats sb = run_with_routing(b.sim, b.routing, b.seed);

  CHECK(a.sim.result_digest() == b.sim.result_digest());
  CHECK(sa.events_processed == sb.events_processed);
  CHECK(collective_time_ns(a.sim.results()) == collective_time_ns(b.sim.results()));
}

TEST_CASE("collective time is zero when any flow fails, because a barrier has no partial credit") {
  AllReduceConfig cfg = cross_pod_ring();
  cfg.topology.host_link.queue_capacity_pkts = 2;
  cfg.topology.fabric_link.queue_capacity_pkts = 2;
  cfg.transport.max_attempts = 1;  // the first drop is fatal

  AllReduceScenario sc = build_all_reduce(cfg);
  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);

  if (stats.flows_incomplete > 0) {
    CHECK(collective_time_ns(sc.sim.results()) == 0);
  }
}
