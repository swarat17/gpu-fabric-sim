#pragma once

#include <cstdint>
#include <string_view>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"
#include "fabric/routing/policy.hpp"
#include "fabric/topology/fat_tree.hpp"
#include "fabric/topology/leaf_spine.hpp"
#include "fabric/workload/permutation.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Closed-form flow completion time for a single flow crossing `hops`
// homogeneous, uncongested links.
//
// Derivation (store-and-forward, source-paced at NIC line rate, all links
// identical). Packet i is released at i*ser and clocked across each of the
// `hops` links in turn; because the release period equals the per-link
// serialisation time, the pipeline never stalls and no queue ever exceeds one
// packet. Packet i therefore lands at (i + hops)*ser + hops*prop, and the flow
// finishes with its last packet, i = n-1:
//
//     FCT = (n - 1 + hops) * ser + hops * prop
//
// This is an exact integer identity, not an approximation, which is why the
// validation test asserts equality rather than a tolerance. See
// docs/validation.md.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr Nanos analytical_fct_ns(std::uint32_t packets, std::uint32_t hops,
                                                Nanos ser_ns, Nanos prop_ns) noexcept {
  return (static_cast<Nanos>(packets) - 1 + static_cast<Nanos>(hops)) * ser_ns +
         static_cast<Nanos>(hops) * prop_ns;
}

// The same identity for a source paced below line rate: the release period is
// `period_ns` rather than `ser_ns`. Reduces to the above when they are equal.
//
// Under any routing policy and any amount of cross traffic this is a *lower
// bound* on a flow's completion time -- congestion can only add queueing delay,
// never remove it. tests/integration/test_permutation.cpp asserts no flow ever
// beats it, which catches the whole class of bugs where a packet skips a hop or
// a link forgets to charge serialisation.
[[nodiscard]] constexpr Nanos paced_fct_ns(std::uint32_t packets, std::uint32_t hops, Nanos ser_ns,
                                           Nanos period_ns, Nanos prop_ns) noexcept {
  return (static_cast<Nanos>(packets) - 1) * period_ns + static_cast<Nanos>(hops) * ser_ns +
         static_cast<Nanos>(hops) * prop_ns;
}

// The project's single runtime routing dispatch. Everything below this call is
// resolved at compile time; see routing/policy.hpp.
RunStats run_with_routing(Simulation& sim, RoutingAlgorithm algo, std::uint64_t hash_seed);

[[nodiscard]] bool parse_routing(std::string_view name, RoutingAlgorithm& out) noexcept;

// ---------------------------------------------------------------------------
// smoke -- one flow on a leaf-spine fabric, checked against the closed form.
// ---------------------------------------------------------------------------
struct SmokeConfig {
  LeafSpineConfig topology{};
  Bytes flow_bytes = 1'500'000;
  std::uint16_t mtu_bytes = 1500;
  std::uint32_t src_host = 0;
  std::uint32_t dst_host = 4;  // on the far leaf with the default 4 hosts/leaf
};

struct SmokeScenario {
  Simulation sim;
  FlowId flow{};
  std::uint32_t packets = 0;
  std::uint32_t hops = 0;
  Nanos analytical_ns = 0;
  // True when the closed form above is exact for this configuration: all links
  // homogeneous, the flow an exact multiple of the MTU, and serialisation
  // dividing without truncation. The validation test refuses to run otherwise
  // rather than silently comparing against a rounded number.
  bool analytical_exact = false;
};

[[nodiscard]] SmokeScenario build_smoke(const SmokeConfig& cfg);

// ---------------------------------------------------------------------------
// permutation -- the M1 scenario. Every host sends one flow to a distinct host
// across a k-ary fat-tree, under a chosen routing policy.
// ---------------------------------------------------------------------------
struct PermutationConfig {
  FatTreeConfig topology{};
  PermutationParams workload{};
  RoutingAlgorithm routing = RoutingAlgorithm::Ecmp;
};

struct PermutationScenario {
  Simulation sim;
  std::uint32_t k = 0;
  std::uint32_t flow_count = 0;
  RoutingAlgorithm routing = RoutingAlgorithm::Ecmp;
  std::uint64_t seed = 0;
  // Best case over all flows: the lower bound for the shortest path present in
  // the workload. Reported next to the measured percentiles so a reader can see
  // how much of the FCT is congestion and how much is just the wire.
  Nanos best_case_ns = 0;
  bool analytical_exact = false;
};

[[nodiscard]] PermutationScenario build_permutation(const PermutationConfig& cfg);

// Uncongested lower bound for one flow of the permutation workload.
[[nodiscard]] Nanos permutation_flow_lower_bound_ns(const PermutationConfig& cfg,
                                                    std::uint32_t src_host,
                                                    std::uint32_t dst_host) noexcept;

}  // namespace fabric
