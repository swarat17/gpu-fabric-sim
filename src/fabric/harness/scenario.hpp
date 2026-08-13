#pragma once

#include <cstdint>
#include <string_view>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"
#include "fabric/routing/policy.hpp"
#include "fabric/topology/fat_tree.hpp"
#include "fabric/topology/leaf_spine.hpp"
#include "fabric/workload/all_reduce.hpp"
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

// ---------------------------------------------------------------------------
// Transport sizing. All three of these are properties of the topology, so the
// harness computes them and hands them to the simulator rather than letting a
// magic constant decide how the experiment behaves.
// ---------------------------------------------------------------------------

// Time from putting a packet on the wire to its ack coming back, uncongested.
[[nodiscard]] constexpr Nanos round_trip_ns(std::uint32_t hops, Nanos ser_ns, Nanos ack_ser_ns,
                                            Nanos prop_ns) noexcept {
  return static_cast<Nanos>(hops) * (ser_ns + prop_ns) +
         static_cast<Nanos>(hops) * (ack_ser_ns + prop_ns);
}

// Smallest window that keeps the pipe full. Below this the *window* is the
// bottleneck, the flow runs slower than the link allows, and the closed form
// stops describing it -- so validation scenarios assert the window exceeds it
// rather than silently reporting a throttled number as a modelling result.
[[nodiscard]] constexpr std::uint32_t min_window_packets(std::uint32_t hops, Nanos ser_ns,
                                                         Nanos ack_ser_ns, Nanos prop_ns) noexcept {
  return static_cast<std::uint32_t>(round_trip_ns(hops, ser_ns, ack_ser_ns, prop_ns) / ser_ns) + 1;
}

// A window that holds the whole flow cannot throttle it either, however small
// the flow is -- so a short transfer needs no bandwidth-delay product at all.
[[nodiscard]] constexpr bool window_is_sufficient(std::uint32_t window_pkts,
                                                  std::uint32_t packets, std::uint32_t hops,
                                                  Nanos ser_ns, Nanos ack_ser_ns,
                                                  Nanos prop_ns) noexcept {
  return window_pkts >= packets ||
         window_pkts >= min_window_packets(hops, ser_ns, ack_ser_ns, prop_ns);
}

// Retransmission timeout, sized to cover a full round trip *plus* a completely
// full buffer at every hop.
//
// Deliberately generous. A timeout that fires while a packet is merely queued
// produces a spurious retransmission, which wastes capacity and -- much worse --
// does so in proportion to how congested a path is. That would systematically
// penalise whichever router leaves packets queued longest, which is exactly the
// quantity under study. Slow recovery from a genuine drop is the cheaper error.
[[nodiscard]] constexpr Nanos default_rto_ns(std::uint32_t hops, Nanos ser_ns, Nanos ack_ser_ns,
                                             Nanos prop_ns, std::uint32_t queue_pkts) noexcept {
  return 2 * (round_trip_ns(hops, ser_ns, ack_ser_ns, prop_ns) +
              static_cast<Nanos>(hops) * static_cast<Nanos>(queue_pkts) * ser_ns);
}

// True when an ack occupies a link for zero nanoseconds, i.e. it propagates and
// queues and can be dropped, but never delays a data packet behind it. Only a
// validation configuration should want this; see docs/validation.md.
[[nodiscard]] constexpr bool ack_is_free(Bytes ack_bytes, BitsPerSec rate) noexcept {
  return serialization_ns(ack_bytes, rate) == 0;
}

// ---------------------------------------------------------------------------
// Ring all-reduce closed form.
//
// The collective is 2(N-1) dependent steps, each one chunk crossing `hops`
// links, so the total is simply the per-step time multiplied out:
//
//     T = 2(N-1) * [ (chunk_packets - 1) * period + hops * ser + hops * prop ]
//
// This is the integer-exact form of the textbook
//
//     T = 2(N-1)*alpha + 2(N-1)/N * S/B
//
// with alpha = hops*(ser + prop) the per-step latency and (chunk-1)*period the
// transfer term. The textbook version drops the store-and-forward pipeline fill
// and rounds the chunk to a real number; this one does neither, which is why it
// can be asserted as an equality.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr Nanos all_reduce_analytical_ns(std::uint32_t steps,
                                                       std::uint32_t chunk_packets,
                                                       std::uint32_t hops, Nanos ser_ns,
                                                       Nanos period_ns, Nanos prop_ns) noexcept {
  return static_cast<Nanos>(steps) * paced_fct_ns(chunk_packets, hops, ser_ns, period_ns, prop_ns);
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
  // Large enough to exceed the bandwidth-delay product of every configuration
  // the validation suite sweeps, including the 5 us propagation delay -- the
  // point of this scenario is the link model, not the window.
  std::uint32_t window_pkts = 512;
};

struct SmokeScenario {
  Simulation sim;
  FlowId flow{};
  std::uint32_t packets = 0;
  std::uint32_t hops = 0;
  Nanos analytical_ns = 0;
  // True when the closed form above is exact for this configuration: all links
  // homogeneous, the flow an exact multiple of the MTU, serialisation dividing
  // without truncation, and a window large enough not to throttle. The
  // validation test refuses to run otherwise rather than silently comparing
  // against a rounded number.
  bool analytical_exact = false;
};

[[nodiscard]] SmokeScenario build_smoke(const SmokeConfig& cfg);

// ---------------------------------------------------------------------------
// permutation -- every host sends one flow to a distinct host across a k-ary
// fat-tree, under a chosen routing policy.
// ---------------------------------------------------------------------------
struct PermutationConfig {
  FatTreeConfig topology{};
  PermutationParams workload{};
  RoutingAlgorithm routing = RoutingAlgorithm::Ecmp;
  TransportConfig transport{};  // rto_ns == 0 means "size it from the topology"
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

// ---------------------------------------------------------------------------
// allreduce -- the headline workload. A ring collective over a k-ary fat-tree.
// ---------------------------------------------------------------------------
struct AllReduceConfig {
  FatTreeConfig topology{};
  AllReduceParams workload{};
  RoutingAlgorithm routing = RoutingAlgorithm::Ecmp;
  TransportConfig transport{};
};

struct AllReduceScenario {
  Simulation sim;
  std::uint32_t gpus = 0;
  std::uint32_t steps = 0;
  std::uint32_t chunk_packets = 0;
  std::uint32_t hops = 0;  // meaningful only when uniform_hops is true
  Bytes chunk_bytes = 0;
  Nanos analytical_ns = 0;
  RoutingAlgorithm routing = RoutingAlgorithm::Ecmp;
  std::uint64_t seed = 0;

  // Every ring hop crosses the same number of links, so one closed form
  // describes every step.
  bool uniform_hops = false;
  // The closed form is an exact identity for this configuration rather than
  // merely a lower bound: uniform hops, exact chunking, exact serialisation, a
  // window that never throttles, links that cannot queue (each carries one
  // flow), and acks that cost no wire time.
  bool analytical_exact = false;
};

[[nodiscard]] AllReduceScenario build_all_reduce(const AllReduceConfig& cfg);

}  // namespace fabric
