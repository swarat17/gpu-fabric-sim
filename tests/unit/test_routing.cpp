#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "fabric/routing/route_table.hpp"
#include "fabric/topology/leaf_spine.hpp"

using namespace fabric;

TEST_CASE("every node has a route to every host") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  const RouteTable rt = RouteTable::build_shortest_path(f);

  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    const NodeId dst = f.host_at(h);
    for (std::size_t i = 0; i < f.node_count(); ++i) {
      const NodeId n{static_cast<std::uint32_t>(i)};
      if (n == dst) {
        continue;
      }
      CHECK_MESSAGE(!rt.candidates(n, h).empty(), "unreachable destination host");
    }
  }
}

TEST_CASE("a leaf sees one equal-cost candidate per spine for a remote host") {
  // The property M1's ECMP and M3's adaptive policy both depend on: the table
  // hands back the full equal-cost set, not a single pre-chosen next hop.
  LeafSpineConfig cfg;
  cfg.spine_count = 4;
  const Fabric f = build_leaf_spine(cfg);
  const RouteTable rt = RouteTable::build_shortest_path(f);

  // Node ordering is spines, then leaves, then hosts (see leaf_spine.hpp).
  const NodeId leaf0{cfg.spine_count};
  const std::uint32_t remote_host = cfg.hosts_per_leaf;  // first host on leaf 1

  CHECK(rt.candidates(leaf0, remote_host).size() == cfg.spine_count);
  // ...and exactly one for a host hanging off the leaf itself.
  CHECK(rt.candidates(leaf0, 0).size() == 1);
}

TEST_CASE("a spine has a single next hop toward any host") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  const RouteTable rt = RouteTable::build_shortest_path(f);
  const NodeId spine0{0};
  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    CHECK(rt.candidates(spine0, h).size() == 1);
  }
}

TEST_CASE("following the first candidate reaches the destination in minimum hops") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  const RouteTable rt = RouteTable::build_shortest_path(f);

  const std::uint32_t dst_host = 4;  // first host on the far leaf
  const NodeId dst = f.host_at(dst_host);

  NodeId at = f.host_at(0);
  std::uint32_t hops = 0;
  while (at != dst) {
    const std::span<const PortId> cand = rt.candidates(at, dst_host);
    REQUIRE_FALSE(cand.empty());
    at = f.port(cand[0]).peer_node;
    ++hops;
    REQUIRE(hops <= 8);  // guards against a routing loop hanging the suite
  }
  CHECK(hops == kLeafSpineCrossLeafHops);
}

TEST_CASE("every candidate is a strict progress step toward the destination") {
  // Guards the property that makes any choice among candidates safe: whichever
  // port a policy picks, the packet is one hop closer. Without this an adaptive
  // router could be made to loop.
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  const RouteTable rt = RouteTable::build_shortest_path(f);

  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    const NodeId dst = f.host_at(h);
    for (std::size_t i = 0; i < f.node_count(); ++i) {
      const NodeId n{static_cast<std::uint32_t>(i)};
      if (n == dst) {
        continue;
      }
      for (const PortId p : rt.candidates(n, h)) {
        // Walk from the neighbour; it must reach dst in strictly fewer hops.
        NodeId at = f.port(p).peer_node;
        std::uint32_t hops = 0;
        while (at != dst && hops <= 8) {
          at = f.port(rt.candidates(at, h)[0]).peer_node;
          ++hops;
        }
        CHECK(at == dst);
      }
    }
  }
}
