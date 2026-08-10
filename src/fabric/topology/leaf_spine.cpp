#include "fabric/topology/leaf_spine.hpp"

#include <cassert>
#include <vector>

namespace fabric {

Fabric build_leaf_spine(const LeafSpineConfig& cfg) {
  assert(cfg.spine_count > 0 && cfg.leaf_count > 0 && cfg.hosts_per_leaf > 0);

  FabricBuilder b;

  std::vector<NodeId> spines;
  spines.reserve(cfg.spine_count);
  for (std::uint32_t i = 0; i < cfg.spine_count; ++i) {
    spines.push_back(b.add_switch());
  }

  std::vector<NodeId> leaves;
  leaves.reserve(cfg.leaf_count);
  for (std::uint32_t i = 0; i < cfg.leaf_count; ++i) {
    leaves.push_back(b.add_switch());
  }

  // Full bipartite leaf<->spine mesh.
  for (const NodeId leaf : leaves) {
    for (const NodeId spine : spines) {
      b.connect(leaf, spine, cfg.fabric_link);
    }
  }

  // Hosts, grouped by leaf, so host dense index h belongs to leaf
  // h / hosts_per_leaf.
  for (const NodeId leaf : leaves) {
    for (std::uint32_t i = 0; i < cfg.hosts_per_leaf; ++i) {
      const NodeId host = b.add_host();
      b.connect(host, leaf, cfg.host_link);
    }
  }

  return b.build();
}

}  // namespace fabric
