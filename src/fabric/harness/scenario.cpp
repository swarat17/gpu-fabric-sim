#include "fabric/harness/scenario.hpp"

#include <cassert>
#include <utility>
#include <vector>

#include "fabric/routing/route_table.hpp"

namespace fabric {

namespace {

// Fills in any transport parameter the caller left at "auto" using the
// topology, so a scenario never runs with an unsized timeout.
TransportConfig sized_transport(const TransportConfig& in, const LinkParams& link,
                                std::uint16_t mtu_bytes, std::uint32_t hops) {
  TransportConfig t = in;
  if (t.rto_ns == 0) {
    const Nanos ser = serialization_ns(mtu_bytes, link.rate);
    const Nanos ack_ser = serialization_ns(t.ack_bytes, link.rate);
    t.rto_ns = default_rto_ns(hops, ser, ack_ser, link.prop_delay_ns,
                              static_cast<std::uint32_t>(link.queue_capacity_pkts));
  }
  return t;
}

}  // namespace

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

  const TransportConfig transport = sized_transport(TransportConfig{}, host_link, cfg.mtu_bytes,
                                                    hops);

  const Nanos ser = serialization_ns(mtu, host_link.rate);
  const Nanos ack_ser = serialization_ns(transport.ack_bytes, host_link.rate);
  const Nanos analytical = analytical_fct_ns(packets, hops, ser, host_link.prop_delay_ns);

  // Only one host sends and only one receives, so the ack stream has the reverse
  // direction of every link to itself -- a directed link is a port, so acks
  // never queue behind the data they acknowledge. That is what keeps this an
  // exact identity even with acks costing real wire time.
  const bool exact = homogeneous && (cfg.flow_bytes % mtu == 0) &&
                     serialization_is_exact(mtu, host_link.rate) &&
                     window_is_sufficient(cfg.window_pkts, packets, hops, ser, ack_ser,
                                          host_link.prop_delay_ns);

  Simulation sim(std::move(fab), std::move(routes));
  sim.set_transport(transport);

  FlowSpec spec;
  spec.src = src;
  spec.dst = dst;
  spec.size_bytes = cfg.flow_bytes;
  spec.mtu_bytes = cfg.mtu_bytes;
  spec.window_pkts = cfg.window_pkts;
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

  const LinkParams& host_link = cfg.topology.host_link;
  const LinkParams& fabric_link = cfg.topology.fabric_link;
  sc.sim.set_transport(sized_transport(cfg.transport, host_link, cfg.workload.mtu_bytes,
                                       kFatTreeCrossPodHops));

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

  const Nanos ser = serialization_ns(cfg.workload.mtu_bytes, host_link.rate);
  const Nanos ack_ser = serialization_ns(sc.sim.transport().ack_bytes, host_link.rate);
  const auto packets =
      static_cast<std::uint32_t>((cfg.workload.flow_bytes + cfg.workload.mtu_bytes - 1) /
                                 cfg.workload.mtu_bytes);
  sc.analytical_exact =
      host_link.rate == fabric_link.rate &&
      host_link.prop_delay_ns == fabric_link.prop_delay_ns &&
      (cfg.workload.flow_bytes % cfg.workload.mtu_bytes == 0) &&
      serialization_is_exact(cfg.workload.mtu_bytes, host_link.rate) &&
      window_is_sufficient(cfg.workload.window_pkts, packets, kFatTreeCrossPodHops, ser, ack_ser,
                           host_link.prop_delay_ns);

  return sc;
}

AllReduceScenario build_all_reduce(const AllReduceConfig& cfg) {
  AllReduceParams params = cfg.workload;
  params.fat_tree_k = cfg.topology.k;

  Fabric fab = build_fat_tree(cfg.topology);
  RouteTable routes = RouteTable::build_shortest_path(fab);

  const AllReduce ar = make_ring_all_reduce(fab, params);

  // Hop count of every ring step. One closed form describes the collective only
  // if they all agree; otherwise the analytical figure is the optimistic case
  // and is reported as a bound.
  std::uint32_t hops = 0;
  std::uint32_t max_hops = 0;
  bool uniform = true;
  for (std::uint32_t r = 0; r < ar.gpus; ++r) {
    const std::uint32_t h = fat_tree_hops(cfg.topology.k, ar.rank_to_host[r],
                                          ar.rank_to_host[(r + 1) % ar.gpus]);
    if (r == 0) {
      hops = h;
    } else if (h != hops) {
      uniform = false;
    }
    if (h > max_hops) {
      max_hops = h;
    }
  }

  const LinkParams& host_link = cfg.topology.host_link;
  const LinkParams& fabric_link = cfg.topology.fabric_link;
  const TransportConfig transport =
      sized_transport(cfg.transport, host_link, params.mtu_bytes, max_hops);

  const Nanos ser = serialization_ns(params.mtu_bytes, host_link.rate);
  const Nanos ack_ser = serialization_ns(transport.ack_bytes, host_link.rate);
  const Nanos period = (ser * 1000ULL) / params.load_permille;

  AllReduceScenario sc{Simulation(std::move(fab), std::move(routes))};
  sc.sim.set_transport(transport);
  sc.gpus = ar.gpus;
  sc.steps = ar.steps;
  sc.chunk_bytes = ar.chunk_bytes;
  sc.chunk_packets = ar.chunk_packets;
  sc.hops = uniform ? hops : max_hops;
  sc.uniform_hops = uniform;
  sc.routing = cfg.routing;
  sc.seed = params.seed;
  sc.analytical_ns = all_reduce_analytical_ns(ar.steps, ar.chunk_packets, sc.hops, ser, period,
                                              host_link.prop_delay_ns);

  // The identity holds only when nothing can queue anywhere. A ring confined to
  // one edge switch is link-disjoint -- each host's uplink carries exactly its
  // own data and each downlink exactly one neighbour's -- so with zero-cost acks
  // and a window that never throttles, the model has no freedom left.
  sc.analytical_exact = uniform && hops == kFatTreeSameEdgeHops && ar.chunk_exact &&
                        host_link.rate == fabric_link.rate &&
                        host_link.prop_delay_ns == fabric_link.prop_delay_ns &&
                        serialization_is_exact(params.mtu_bytes, host_link.rate) &&
                        ack_is_free(transport.ack_bytes, host_link.rate) &&
                        (ser * 1000ULL) % params.load_permille == 0 &&
                        window_is_sufficient(params.window_pkts, ar.chunk_packets, hops, ser,
                                             ack_ser, host_link.prop_delay_ns);

  for (const FlowSpec& s : ar.flows) {
    sc.sim.add_flow(s);
  }

  return sc;
}

}  // namespace fabric
