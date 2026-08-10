#include "fabric/harness/scenario.hpp"

#include <cassert>
#include <utility>

#include "fabric/routing/route_table.hpp"

namespace fabric {

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
  const FlowId flow = sim.add_flow(FlowSpec{src, dst, cfg.flow_bytes, 0, cfg.mtu_bytes});

  return SmokeScenario{std::move(sim), flow, packets, hops, analytical, exact};
}

}  // namespace fabric
