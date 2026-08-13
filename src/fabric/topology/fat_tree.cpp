#include "fabric/topology/fat_tree.hpp"

#include <cassert>

namespace fabric {

Fabric build_fat_tree(const FatTreeConfig& cfg) {
  const std::uint32_t k = cfg.k;
  assert(k >= 2 && k % 2 == 0 && "fat-tree arity must be even");

  const std::uint32_t half = k / 2;
  FabricBuilder b;

  // Nodes first, in the order documented in fat_tree.hpp, so that the id
  // arithmetic there is valid. Edges are recorded afterwards; FabricBuilder
  // materialises ports in a second pass regardless.
  for (std::uint32_t c = 0; c < fat_tree_core_count(k); ++c) {
    [[maybe_unused]] const NodeId id = b.add_switch();
    assert(id == NodeId{c});
  }
  for (std::uint32_t pod = 0; pod < k; ++pod) {
    for (std::uint32_t a = 0; a < half; ++a) {
      [[maybe_unused]] const NodeId id = b.add_switch();
      assert(id == fat_tree_agg(k, pod, a));
    }
    for (std::uint32_t e = 0; e < half; ++e) {
      [[maybe_unused]] const NodeId id = b.add_switch();
      assert(id == fat_tree_edge(k, pod, e));
    }
  }

  const std::uint32_t first_host = fat_tree_switch_count(k);
  for (std::uint32_t h = 0; h < fat_tree_host_count(k); ++h) {
    [[maybe_unused]] const NodeId id = b.add_host();
    assert(id == NodeId{first_host + h});
  }

  for (std::uint32_t pod = 0; pod < k; ++pod) {
    for (std::uint32_t a = 0; a < half; ++a) {
      const NodeId agg = fat_tree_agg(k, pod, a);

      // Up: aggregation switch a of every pod attaches to the core row a.
      // Row-per-aggregation-index is what makes the fabric rearrangeably
      // non-blocking rather than merely well connected.
      for (std::uint32_t j = 0; j < half; ++j) {
        b.connect(agg, fat_tree_core(k, a, j), cfg.fabric_link);
      }
      // Down: full bipartite mesh with the edge switches of the same pod.
      for (std::uint32_t e = 0; e < half; ++e) {
        b.connect(agg, fat_tree_edge(k, pod, e), cfg.fabric_link);
      }
    }
  }

  for (std::uint32_t h = 0; h < fat_tree_host_count(k); ++h) {
    const std::uint32_t pod = fat_tree_pod_of_host(k, h);
    const std::uint32_t e = (h / half) % half;
    b.connect(NodeId{first_host + h}, fat_tree_edge(k, pod, e), cfg.host_link);
  }

  return b.build();
}

}  // namespace fabric
