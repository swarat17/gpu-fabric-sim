#pragma once

#include <cstdint>

#include "fabric/model/fabric.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Two-tier leaf/spine -- the M0 topology.
//
// Every leaf connects to every spine, so a leaf-to-leaf path has exactly
// `spine_count` equal-cost options. That is deliberate: it gives M1's ECMP and
// M3's adaptive policy something to choose between on a topology small enough
// to reason about by hand, before the k-ary fat-tree lands in M1.
//
// Node ordering is fixed and documented because tests and scenarios address
// hosts by dense index: spines first, then leaves, then hosts grouped by leaf.
// ---------------------------------------------------------------------------
struct LeafSpineConfig {
  std::uint32_t spine_count = 2;
  std::uint32_t leaf_count = 2;
  std::uint32_t hosts_per_leaf = 4;
  LinkParams host_link{};    // host NIC <-> leaf
  LinkParams fabric_link{};  // leaf <-> spine
};

[[nodiscard]] Fabric build_leaf_spine(const LeafSpineConfig& cfg);

// Hop count between two hosts on different leaves: host->leaf->spine->leaf->host.
inline constexpr std::uint32_t kLeafSpineCrossLeafHops = 4;
// Hop count between two hosts on the same leaf: host->leaf->host.
inline constexpr std::uint32_t kLeafSpineSameLeafHops = 2;

}  // namespace fabric
