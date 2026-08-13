#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fabric/routing/route_table.hpp"
#include "fabric/topology/fat_tree.hpp"

using namespace fabric;

namespace {

FatTreeConfig cfg_for(std::uint32_t k) {
  FatTreeConfig cfg;
  cfg.k = k;
  return cfg;
}

// Number of distinct minimum-hop paths from `at` to host `dst_host`, walking the
// equal-cost candidate sets. Exponential in principle; the fat-trees under test
// are small and the depth is at most 6.
std::uint32_t count_paths(const Fabric& f, const RouteTable& rt, NodeId at,
                          std::uint32_t dst_host) {
  if (at == f.host_at(dst_host)) {
    return 1;
  }
  std::uint32_t total = 0;
  for (const PortId p : rt.candidates(at, dst_host)) {
    total += count_paths(f, rt, f.port(p).peer_node, dst_host);
  }
  return total;
}

}  // namespace

TEST_CASE("a k-ary fat-tree has k^3/4 hosts and 5k^2/4 switches") {
  for (const std::uint32_t k : {2u, 4u, 6u, 8u}) {
    const Fabric f = build_fat_tree(cfg_for(k));
    CAPTURE(k);
    CHECK(f.host_count() == fat_tree_host_count(k));
    CHECK(f.node_count() == fat_tree_host_count(k) + fat_tree_switch_count(k));
  }
}

TEST_CASE("every switch has exactly k ports and every host exactly one") {
  // The defining property of a fat-tree: it is built from k-port switches only,
  // which is what made it interesting versus a fat root switch in the first
  // place. A wiring bug shows up here before it shows up as a routing anomaly.
  for (const std::uint32_t k : {4u, 6u, 8u}) {
    const Fabric f = build_fat_tree(cfg_for(k));
    CAPTURE(k);
    for (std::size_t i = 0; i < f.node_count(); ++i) {
      const NodeId n{static_cast<std::uint32_t>(i)};
      const std::size_t degree = f.ports_of(n).size();
      if (f.node(n).is_host) {
        CHECK(degree == 1);
      } else {
        CHECK(degree == k);
      }
    }
  }
}

TEST_CASE("every directed port pairs back with matching link parameters") {
  const Fabric f = build_fat_tree(cfg_for(4));
  for (std::size_t i = 0; i < f.port_count(); ++i) {
    const PortId p{static_cast<std::uint32_t>(i)};
    const Port& fwd = f.port(p);
    const Port& rev = f.port(fwd.peer_port);
    CHECK(rev.peer_port == p);
    CHECK(rev.owner == fwd.peer_node);
    CHECK(rev.rate == fwd.rate);
    CHECK(rev.prop_delay_ns == fwd.prop_delay_ns);
  }
}

TEST_CASE("hosts are grouped under their edge switch in dense index order") {
  const std::uint32_t k = 4;
  const Fabric f = build_fat_tree(cfg_for(k));
  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    const NodeId host = f.host_at(h);
    CAPTURE(h);
    CHECK(f.host_index(host) == h);
    const NodeId edge = f.port(f.ports_of(host)[0]).peer_node;
    const std::uint32_t pod = fat_tree_pod_of_host(k, h);
    const std::uint32_t within_pod = (h / (k / 2)) % (k / 2);
    CHECK(edge == fat_tree_edge(k, pod, within_pod));
  }
}

TEST_CASE("equal-cost breadth matches the tier the packet is on") {
  // This is the property both routing policies live off: an edge switch heading
  // out of its pod really does see k/2 usable uplinks, and an aggregation switch
  // really does see k/2 cores. If either collapsed to one candidate, ECMP and
  // the adaptive router would be the same function.
  const std::uint32_t k = 4;
  const Fabric f = build_fat_tree(cfg_for(k));
  const RouteTable rt = RouteTable::build_shortest_path(f);

  const std::uint32_t local_host = 0;                        // pod 0, edge 0
  const std::uint32_t same_pod_other_edge = k / 2;           // pod 0, edge 1
  const std::uint32_t remote_host = fat_tree_hosts_per_pod(k);  // pod 1

  const NodeId edge00 = fat_tree_edge(k, 0, 0);
  CHECK(rt.candidates(edge00, local_host).size() == 1);              // straight down
  CHECK(rt.candidates(edge00, same_pod_other_edge).size() == k / 2);  // up to any agg
  CHECK(rt.candidates(edge00, remote_host).size() == k / 2);          // up to any agg

  const NodeId agg00 = fat_tree_agg(k, 0, 0);
  CHECK(rt.candidates(agg00, remote_host).size() == k / 2);  // up to any core
  CHECK(rt.candidates(agg00, local_host).size() == 1);       // down to the edge

  const NodeId core0 = fat_tree_core(k, 0, 0);
  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    CHECK(rt.candidates(core0, h).size() == 1);  // one aggregation switch per pod
  }
}

TEST_CASE("cross-pod host pairs have k^2/4 equal-cost paths") {
  // Full bisection bandwidth expressed as a path count -- the number the ECMP
  // hash has to spread over.
  for (const std::uint32_t k : {4u, 6u}) {
    const Fabric f = build_fat_tree(cfg_for(k));
    const RouteTable rt = RouteTable::build_shortest_path(f);
    const std::uint32_t remote = fat_tree_hosts_per_pod(k);
    CAPTURE(k);
    CHECK(count_paths(f, rt, f.host_at(0), remote) == fat_tree_cross_pod_paths(k));
    // Same pod, different edge switch: k/2 paths, one per aggregation switch.
    CHECK(count_paths(f, rt, f.host_at(0), k / 2) == k / 2);
    // Same edge switch: exactly one.
    CHECK(count_paths(f, rt, f.host_at(0), 1) == 1);
  }
}

TEST_CASE("every candidate path has the analytical hop count") {
  const std::uint32_t k = 4;
  const Fabric f = build_fat_tree(cfg_for(k));
  const RouteTable rt = RouteTable::build_shortest_path(f);

  for (std::uint32_t src = 0; src < f.host_count(); ++src) {
    for (std::uint32_t dst = 0; dst < f.host_count(); ++dst) {
      if (src == dst) {
        continue;
      }
      NodeId at = f.host_at(src);
      std::uint32_t hops = 0;
      while (at != f.host_at(dst)) {
        const std::span<const PortId> cand = rt.candidates(at, dst);
        REQUIRE_FALSE(cand.empty());
        at = f.port(cand[0]).peer_node;
        ++hops;
        REQUIRE(hops <= 10);  // guards against a routing loop hanging the suite
      }
      CHECK(hops == fat_tree_hops(k, src, dst));
    }
  }
}

TEST_CASE("every candidate on a fat-tree is a strict step toward the destination") {
  // Loop freedom, restated for the topology every published number uses.
  const std::uint32_t k = 4;
  const Fabric f = build_fat_tree(cfg_for(k));
  const RouteTable rt = RouteTable::build_shortest_path(f);

  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    const NodeId dst = f.host_at(h);
    for (std::size_t i = 0; i < f.node_count(); ++i) {
      const NodeId n{static_cast<std::uint32_t>(i)};
      if (n == dst) {
        continue;
      }
      const std::span<const PortId> cand = rt.candidates(n, h);
      REQUIRE_FALSE(cand.empty());
      for (const PortId p : cand) {
        NodeId at = f.port(p).peer_node;
        std::uint32_t hops = 0;
        while (at != dst && hops <= 10) {
          at = f.port(rt.candidates(at, h)[0]).peer_node;
          ++hops;
        }
        CHECK(at == dst);
      }
    }
  }
}
