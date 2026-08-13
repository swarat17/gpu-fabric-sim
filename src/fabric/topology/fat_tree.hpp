#pragma once

#include <cstdint>

#include "fabric/core/ids.hpp"
#include "fabric/model/fabric.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// k-ary fat-tree (Al-Fares et al., SIGCOMM'08) -- the M1 topology and the one
// every published number is measured on.
//
// Every switch has exactly k ports. There are k pods; each pod holds k/2
// aggregation and k/2 edge switches. Each edge switch takes k/2 hosts below and
// k/2 aggregation switches above; each aggregation switch takes k/2 core
// switches above. That gives (k/2)^2 cores, 5k^2/4 switches and k^3/4 hosts,
// with full bisection bandwidth: k^2/4 equal-cost paths between any two pods.
//
// Those paths are the entire point. ECMP picks among them by hashing; M3's
// adaptive policy picks among them by observed congestion. A topology with one
// path would make both routers identical and the project pointless.
//
// The index arithmetic below is constexpr because it genuinely is compile-time
// -- it lets the invariants be static_asserted rather than tested at runtime.
//
// Node ordering is fixed and part of the interface, because scenarios and tests
// address switches and hosts by dense index:
//
//   [0, k^2/4)                        core switches, core(i, j) = i*(k/2) + j
//   then, per pod p in [0, k):        k/2 aggregation, then k/2 edge switches
//   then                              hosts, grouped by pod then by edge switch
//
// So host dense index h lives in pod h/(k^2/4), under global edge switch
// h/(k/2). Host dense index equals the Fabric's host index by construction.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint32_t fat_tree_host_count(std::uint32_t k) noexcept {
  return k * k * k / 4;
}
[[nodiscard]] constexpr std::uint32_t fat_tree_core_count(std::uint32_t k) noexcept {
  return k * k / 4;
}
[[nodiscard]] constexpr std::uint32_t fat_tree_switch_count(std::uint32_t k) noexcept {
  return 5 * k * k / 4;
}
[[nodiscard]] constexpr std::uint32_t fat_tree_hosts_per_edge(std::uint32_t k) noexcept {
  return k / 2;
}
[[nodiscard]] constexpr std::uint32_t fat_tree_hosts_per_pod(std::uint32_t k) noexcept {
  return k * k / 4;
}

// Node ids. See the ordering comment above.
[[nodiscard]] constexpr NodeId fat_tree_core(std::uint32_t k, std::uint32_t i,
                                             std::uint32_t j) noexcept {
  return NodeId{i * (k / 2) + j};
}
[[nodiscard]] constexpr NodeId fat_tree_agg(std::uint32_t k, std::uint32_t pod,
                                            std::uint32_t a) noexcept {
  return NodeId{fat_tree_core_count(k) + pod * k + a};
}
[[nodiscard]] constexpr NodeId fat_tree_edge(std::uint32_t k, std::uint32_t pod,
                                             std::uint32_t e) noexcept {
  return NodeId{fat_tree_core_count(k) + pod * k + k / 2 + e};
}

[[nodiscard]] constexpr std::uint32_t fat_tree_pod_of_host(std::uint32_t k,
                                                           std::uint32_t host) noexcept {
  return host / fat_tree_hosts_per_pod(k);
}
// Global edge-switch index (0 .. k^2/2), not the within-pod index.
[[nodiscard]] constexpr std::uint32_t fat_tree_edge_of_host(std::uint32_t k,
                                                            std::uint32_t host) noexcept {
  return host / fat_tree_hosts_per_edge(k);
}

// Minimum hop count (links traversed) between two hosts. Three cases only,
// which is what makes a fat-tree tractable to validate against a closed form.
inline constexpr std::uint32_t kFatTreeSameEdgeHops = 2;  // host-edge-host
inline constexpr std::uint32_t kFatTreeSamePodHops = 4;   // host-edge-agg-edge-host
inline constexpr std::uint32_t kFatTreeCrossPodHops = 6;  // ...-agg-core-agg-...

[[nodiscard]] constexpr std::uint32_t fat_tree_hops(std::uint32_t k, std::uint32_t src_host,
                                                    std::uint32_t dst_host) noexcept {
  if (fat_tree_edge_of_host(k, src_host) == fat_tree_edge_of_host(k, dst_host)) {
    return kFatTreeSameEdgeHops;
  }
  if (fat_tree_pod_of_host(k, src_host) == fat_tree_pod_of_host(k, dst_host)) {
    return kFatTreeSamePodHops;
  }
  return kFatTreeCrossPodHops;
}

// Equal-cost path count between hosts in different pods: k/2 aggregation
// choices, each with k/2 core choices. This is the number the ECMP hash has to
// spread flows over, and the number the adaptive router gets to choose from.
[[nodiscard]] constexpr std::uint32_t fat_tree_cross_pod_paths(std::uint32_t k) noexcept {
  return k * k / 4;
}

// Invariants checked by the compiler rather than by a test.
static_assert(fat_tree_host_count(4) == 16);
static_assert(fat_tree_switch_count(4) == 20);
static_assert(fat_tree_core_count(4) == 4);
static_assert(fat_tree_host_count(8) == 128);
static_assert(fat_tree_switch_count(8) == 80);
static_assert(fat_tree_cross_pod_paths(8) == 16);
static_assert(fat_tree_pod_of_host(8, 0) == 0 && fat_tree_pod_of_host(8, 16) == 1);
static_assert(fat_tree_hops(8, 0, 1) == kFatTreeSameEdgeHops);
static_assert(fat_tree_hops(8, 0, 4) == kFatTreeSamePodHops);
static_assert(fat_tree_hops(8, 0, 16) == kFatTreeCrossPodHops);

struct FatTreeConfig {
  std::uint32_t k = 4;       // must be even and >= 2
  LinkParams host_link{};    // host NIC <-> edge switch
  LinkParams fabric_link{};  // edge <-> agg and agg <-> core
};

// The headline claim is measured on k = 8: 128 hosts, 80 switches, 16 equal-cost
// paths between pods.
[[nodiscard]] Fabric build_fat_tree(const FatTreeConfig& cfg);

}  // namespace fabric
