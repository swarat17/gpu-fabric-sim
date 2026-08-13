#include "fabric/harness/scenario.hpp"

#include <cassert>
#include <utility>
#include <vector>

#include "fabric/routing/route_table.hpp"

namespace fabric {

RunStats run_with_routing(Simulation& sim, RoutingAlgorithm algo, std::uint64_t hash_seed) {
  // The only runtime routing dispatch in the project: one switch, once per run,
  // turning a CLI string into a type. Every per-packet decision below this
  // point is a direct call.
  switch (algo) {
    case RoutingAlgorithm::StaticFirst:
      return sim.run(StaticFirstPolicy{});
    case RoutingAlgorithm::Ecmp:
      return sim.run(EcmpPolicy{hash_seed});
  }
  assert(false && "unhandled routing algorithm");
  return RunStats{};
}

bool parse_routing(std::string_view name, RoutingAlgorithm& out) noexcept {
  if (name == "ecmp") {
    out = RoutingAlgorithm::Ecmp;
    return true;
  }
  if (name == "static") {
    out = RoutingAlgorithm::StaticFirst;
    return true;
  }
  return false;
}

SmokeScenario build_smoke(const SmokeConfig& cfg) {
  Fabric fab = build_leaf_spine(cfg.topology);
  RouteTable routes = RouteTable::build_shortest_path(fab);

  assert(cfg.src_host < fab.host_count() && cfg.dst_host < fab.host_count());
  const NodeId src = fab.host_at(cfg.src_host);
  const NodeId dst = fab.host_at(cfg.dst_host);

  const bool same_leaf =
      (cfg.src_host / cfg.topology.hosts_per_leaf) == (cfg.dst_host / cfg.topology.hosts_per_leaf);
  const std::uint32_t hops = same_leaf ? kLeafSpineSameLeafHops : kLeafSpineCrossLeafHops;

  const LinkParams& host_link = cfg.topology.host_link;
  const LinkParams& fabric_link = cfg.topology.fabric_link;
  const bool homogeneous = host_link.rate == fabric_link.rate &&
                           host_link.prop_delay_ns == fabric_link.prop_delay_ns;

  const Bytes mtu = cfg.mtu_bytes;
  const auto packets = static_cast<std::uint32_t>((cfg.flow_bytes + mtu - 1) / mtu);

  const Nanos ser = serialization_ns(mtu, host_link.rate);
  const Nanos analytical = analytical_fct_ns(packets, hops, ser, host_link.prop_delay_ns);
  const bool exact = homogeneous && (cfg.flow_bytes % mtu == 0) &&
                     serialization_is_exact(mtu, host_link.rate);

  Simulation sim(std::move(fab), std::move(routes));
  FlowSpec spec;
  spec.src = src;
  spec.dst = dst;
  spec.size_bytes = cfg.flow_bytes;
  spec.mtu_bytes = cfg.mtu_bytes;
  const FlowId flow = sim.add_flow(spec);

  return SmokeScenario{std::move(sim), flow, packets, hops, analytical, exact};
}

Nanos permutation_flow_lower_bound_ns(const PermutationConfig& cfg, std::uint32_t src_host,
                                      std::uint32_t dst_host) noexcept {
  const PermutationParams& w = cfg.workload;
  const Bytes mtu = w.mtu_bytes;
  const auto packets = static_cast<std::uint32_t>((w.flow_bytes + mtu - 1) / mtu);
  const std::uint32_t hops = fat_tree_hops(cfg.topology.k, src_host, dst_host);

  const Nanos ser = serialization_ns(mtu, cfg.topology.host_link.rate);
  const Nanos period = (ser * 1000ULL) / w.load_permille;
  return paced_fct_ns(packets, hops, ser, period, cfg.topology.host_link.prop_delay_ns);
}

PermutationScenario build_permutation(const PermutationConfig& cfg) {
  Fabric fab = build_fat_tree(cfg.topology);
  RouteTable routes = RouteTable::build_shortest_path(fab);

  const std::vector<FlowSpec> flows = make_permutation(fab, cfg.workload);

  PermutationScenario sc{Simulation(std::move(fab), std::move(routes))};
  sc.k = cfg.topology.k;
  sc.routing = cfg.routing;
  sc.seed = cfg.workload.seed;
  sc.flow_count = static_cast<std::uint32_t>(flows.size());

  Nanos best = 0;
  for (std::uint32_t i = 0; i < flows.size(); ++i) {
    const std::uint32_t dst_host = sc.sim.fabric().host_index(flows[i].dst);
    const Nanos lb = permutation_flow_lower_bound_ns(cfg, i, dst_host);
    if (best == 0 || lb < best) {
      best = lb;
    }
    sc.sim.add_flow(flows[i]);
  }
  sc.best_case_ns = best;

  const LinkParams& host_link = cfg.topology.host_link;
  const LinkParams& fabric_link = cfg.topology.fabric_link;
  sc.analytical_exact = host_link.rate == fabric_link.rate &&
                        host_link.prop_delay_ns == fabric_link.prop_delay_ns &&
                        (cfg.workload.flow_bytes % cfg.workload.mtu_bytes == 0) &&
                        serialization_is_exact(cfg.workload.mtu_bytes, host_link.rate);

  return sc;
}

}  // namespace fabric
