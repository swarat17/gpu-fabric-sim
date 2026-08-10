#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "fabric/topology/leaf_spine.hpp"

using namespace fabric;

TEST_CASE("default leaf-spine has the expected node and port counts") {
  const LeafSpineConfig cfg;  // 2 spine, 2 leaf, 4 hosts per leaf
  const Fabric f = build_leaf_spine(cfg);

  CHECK(f.host_count() == 8);
  CHECK(f.node_count() == 12);  // 2 spine + 2 leaf + 8 hosts
  // 4 leaf<->spine links plus 8 host<->leaf links, two directed ports each.
  CHECK(f.port_count() == 24);
}

TEST_CASE("port accounting holds for wider configurations") {
  LeafSpineConfig cfg;
  cfg.spine_count = 4;
  cfg.leaf_count = 3;
  cfg.hosts_per_leaf = 6;

  const Fabric f = build_leaf_spine(cfg);
  const std::size_t hosts = 3 * 6;
  const std::size_t links = 4 * 3 + hosts;

  CHECK(f.host_count() == hosts);
  CHECK(f.node_count() == 4 + 3 + hosts);
  CHECK(f.port_count() == 2 * links);
}

TEST_CASE("every host has exactly one NIC") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  for (const NodeId h : f.hosts()) {
    CHECK(f.ports_of(h).size() == 1);
    CHECK(f.node(h).is_host);
  }
}

TEST_CASE("each directed port pairs with a reverse port") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  for (std::size_t i = 0; i < f.port_count(); ++i) {
    const PortId p{static_cast<std::uint32_t>(i)};
    const Port& fwd = f.port(p);
    REQUIRE(fwd.owner.valid());
    REQUIRE(fwd.peer_node.valid());
    REQUIRE(fwd.peer_port.valid());

    // A link exists only because two directed ports point at each other; if
    // this ever fails, routing would send packets into a one-way street.
    const Port& rev = f.port(fwd.peer_port);
    CHECK(rev.peer_port == p);
    CHECK(rev.owner == fwd.peer_node);
    CHECK(rev.peer_node == fwd.owner);
    CHECK(rev.rate == fwd.rate);
    CHECK(rev.prop_delay_ns == fwd.prop_delay_ns);
  }
}

TEST_CASE("the ports of a node occupy one contiguous run") {
  // This is the whole point of the two-phase FabricBuilder: a hop looks up
  // ports_of(node) and must not pay a cache miss per candidate.
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  for (std::size_t i = 0; i < f.node_count(); ++i) {
    const NodeId n{static_cast<std::uint32_t>(i)};
    const std::span<const PortId> ports = f.ports_of(n);
    for (std::size_t k = 0; k < ports.size(); ++k) {
      CHECK(f.port(ports[k]).owner == n);
      if (k > 0) {
        CHECK(ports[k].value() == ports[k - 1].value() + 1);
      }
    }
  }
}

TEST_CASE("host dense indexing round-trips") {
  const Fabric f = build_leaf_spine(LeafSpineConfig{});
  for (std::uint32_t h = 0; h < f.host_count(); ++h) {
    CHECK(f.host_index(f.host_at(h)) == h);
  }
}
