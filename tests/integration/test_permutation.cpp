#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

#include "fabric/harness/scenario.hpp"
#include "fabric/metrics/fct.hpp"

using namespace fabric;

namespace {

PermutationConfig config(std::uint32_t k, RoutingAlgorithm routing, std::uint64_t seed,
                         std::uint32_t load_permille = 1000) {
  PermutationConfig cfg;
  cfg.topology.k = k;
  cfg.routing = routing;
  cfg.workload.seed = seed;
  cfg.workload.flow_bytes = 300'000;  // 200 packets: enough to congest, quick to run
  cfg.workload.load_permille = load_permille;
  cfg.workload.window_pkts = 32;
  return cfg;
}

// Every host both sends and receives here, so acks share the sender's uplink
// with its data and a flow can be delayed by an ack being clocked ahead of it.
// Where a test asserts an exact identity rather than a bound, it gives acks zero
// wire time so that the identity is about the network model and nothing else.
PermutationConfig config_free_acks(std::uint32_t k, RoutingAlgorithm routing, std::uint64_t seed,
                                   std::uint32_t load_permille) {
  PermutationConfig cfg = config(k, routing, seed, load_permille);
  cfg.transport.ack_bytes = 1;  // serialises to zero nanoseconds at these rates
  return cfg;
}

}  // namespace

TEST_CASE("the workload really is a permutation") {
  const PermutationConfig cfg = config(4, RoutingAlgorithm::Ecmp, 3);
  PermutationScenario sc = build_permutation(cfg);
  const Fabric& f = sc.sim.fabric();

  REQUIRE(sc.flow_count == f.host_count());

  std::set<std::uint32_t> sources;
  std::set<std::uint32_t> destinations;
  for (const FlowSpec& s : sc.sim.specs()) {
    const std::uint32_t src = f.host_index(s.src);
    const std::uint32_t dst = f.host_index(s.dst);
    CHECK(src != dst);  // Sattolo guarantees a derangement
    sources.insert(src);
    destinations.insert(dst);
  }
  CHECK(sources.size() == f.host_count());
  CHECK(destinations.size() == f.host_count());
}

TEST_CASE("bytes are conserved: injected equals delivered plus dropped") {
  for (const RoutingAlgorithm algo : {RoutingAlgorithm::Ecmp, RoutingAlgorithm::StaticFirst}) {
    PermutationScenario sc = build_permutation(config(4, algo, 1));
    const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);
    CAPTURE(routing_name(algo));
    CHECK(stats.bytes_injected == stats.bytes_delivered + stats.bytes_dropped);
    CHECK(stats.ack_bytes_injected == stats.ack_bytes_delivered + stats.ack_bytes_dropped);
    // At least the offered bytes; more if anything had to be retransmitted.
    CHECK(stats.bytes_injected >= static_cast<Bytes>(sc.flow_count) * 300'000);
  }
}

TEST_CASE("no flow finishes before its uncongested lower bound") {
  // The strongest cheap correctness check on the whole model: congestion can
  // only ever add delay. A packet that skipped a hop, a link that forgot to
  // charge serialisation, or a routing table that took a short cut would all
  // show up as a flow beating the closed form.
  const PermutationConfig cfg = config(4, RoutingAlgorithm::Ecmp, 5);
  PermutationScenario sc = build_permutation(cfg);
  REQUIRE(sc.analytical_exact);

  run_with_routing(sc.sim, sc.routing, sc.seed);

  const Fabric& f = sc.sim.fabric();
  const std::span<const FlowSpec> specs = sc.sim.specs();
  const std::span<const FlowResult> results = sc.sim.results();

  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (!results[i].complete) {
      continue;
    }
    const std::uint32_t src = f.host_index(specs[i].src);
    const std::uint32_t dst = f.host_index(specs[i].dst);
    const Nanos bound = permutation_flow_lower_bound_ns(cfg, src, dst);
    CAPTURE(i);
    CAPTURE(bound);
    CAPTURE(results[i].fct_ns());
    CHECK(results[i].fct_ns() >= bound);
  }
}

TEST_CASE("the shortest path in the workload achieves the lower bound at low load") {
  // At a low enough offered load nothing queues anywhere, so the best flow must
  // match the closed form exactly -- not approximately. If it does not, the
  // model is charging a delay it should not.
  const PermutationConfig cfg = config_free_acks(4, RoutingAlgorithm::Ecmp, 5, 250);
  PermutationScenario sc = build_permutation(cfg);
  REQUIRE(sc.analytical_exact);

  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);
  REQUIRE(stats.bytes_dropped == 0);
  REQUIRE(stats.flows_incomplete == 0);

  const FctSummary fct = summarize_fct(sc.sim.results());
  CHECK(fct.min_ns == sc.best_case_ns);
}

TEST_CASE("ECMP and static routing see byte-identical workloads for a given seed") {
  // The precondition for the paired comparison in M4: the two arms must differ
  // only in the routing decision. If the workload generator consumed randomness
  // differently per policy, every reported difference would be contaminated.
  PermutationScenario a = build_permutation(config(4, RoutingAlgorithm::Ecmp, 9));
  PermutationScenario b = build_permutation(config(4, RoutingAlgorithm::StaticFirst, 9));

  const std::span<const FlowSpec> sa = a.sim.specs();
  const std::span<const FlowSpec> sb = b.sim.specs();
  REQUIRE(sa.size() == sb.size());
  for (std::size_t i = 0; i < sa.size(); ++i) {
    CAPTURE(i);
    CHECK(sa[i].src == sb[i].src);
    CHECK(sa[i].dst == sb[i].dst);
    CHECK(sa[i].src_port == sb[i].src_port);
    CHECK(sa[i].size_bytes == sb[i].size_bytes);
    CHECK(sa[i].start_ns == sb[i].start_ns);
  }
}

TEST_CASE("a multi-flow ECMP run is reproducible") {
  PermutationScenario a = build_permutation(config(4, RoutingAlgorithm::Ecmp, 11));
  PermutationScenario b = build_permutation(config(4, RoutingAlgorithm::Ecmp, 11));

  const RunStats sa = run_with_routing(a.sim, a.routing, a.seed);
  const RunStats sb = run_with_routing(b.sim, b.routing, b.seed);

  CHECK(a.sim.result_digest() == b.sim.result_digest());
  CHECK(sa.events_processed == sb.events_processed);
  CHECK(sa.virtual_end_ns == sb.virtual_end_ns);
  CHECK(sa.bytes_dropped == sb.bytes_dropped);
}

TEST_CASE("a different seed produces a different run") {
  PermutationScenario a = build_permutation(config(4, RoutingAlgorithm::Ecmp, 11));
  PermutationScenario b = build_permutation(config(4, RoutingAlgorithm::Ecmp, 12));

  run_with_routing(a.sim, a.routing, a.seed);
  run_with_routing(b.sim, b.routing, b.seed);

  CHECK(a.sim.result_digest() != b.sim.result_digest());
}

TEST_CASE("ECMP beats single-path routing, which is the point of having paths") {
  // Not a research result -- a wiring check. StaticFirst funnels every cross-pod
  // flow through the same aggregation and core switch, so if ECMP did not do
  // dramatically better, the candidate sets would not be reaching the policy.
  const std::uint32_t k = 4;
  PermutationScenario ecmp = build_permutation(config(k, RoutingAlgorithm::Ecmp, 2, 500));
  PermutationScenario stat = build_permutation(config(k, RoutingAlgorithm::StaticFirst, 2, 500));

  const RunStats se = run_with_routing(ecmp.sim, ecmp.routing, ecmp.seed);
  const RunStats ss = run_with_routing(stat.sim, stat.routing, stat.seed);

  CHECK(se.bytes_dropped <= ss.bytes_dropped);
  CHECK(se.virtual_end_ns < ss.virtual_end_ns);
}
