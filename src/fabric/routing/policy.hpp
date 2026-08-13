#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"
#include "fabric/routing/flow_key.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// How a packet chooses among equal-cost next hops.
//
// The RouteTable hands every policy the *same* full equal-cost candidate span,
// and every candidate is a strict step toward the destination (asserted in
// tests/unit/test_routing.cpp), so no policy -- however it chooses -- can build
// a loop. That property is what makes swapping policies safe.
//
// Dispatch is a compile-time template parameter of Simulation::run, not a
// virtual call. At one routing decision per packet per hop this is the hottest
// branch in the program, and a policy chosen once per run has no business being
// re-resolved a hundred million times. Exactly one runtime `switch` exists, at
// the harness boundary, where the CLI flag is turned into a type.
//
// The `now_ns` parameter is unused by the two policies here and deliberately
// present anyway: M3's flowlet-based adaptive policy needs the arrival time to
// detect an inter-packet gap, and changing the interface later would churn
// every call site.
// ---------------------------------------------------------------------------
template <class P>
concept RoutingPolicy =
    requires(const P& p, NodeId at, const FlowKey& key, std::span<const PortId> cand, Nanos now) {
      { p.select(at, key, cand, now) } noexcept -> std::same_as<PortId>;
    };

enum class RoutingAlgorithm : std::uint8_t {
  // Always the first candidate. Not a routing strategy -- it is the M0
  // single-path behaviour, kept because the analytical validation needs a
  // predetermined path to compare against a closed form.
  StaticFirst = 0,
  Ecmp = 1,
};

[[nodiscard]] constexpr std::string_view routing_name(RoutingAlgorithm a) noexcept {
  switch (a) {
    case RoutingAlgorithm::StaticFirst:
      return "static";
    case RoutingAlgorithm::Ecmp:
      return "ecmp";
  }
  return "?";
}

class StaticFirstPolicy {
 public:
  [[nodiscard]] PortId select(NodeId, const FlowKey&, std::span<const PortId> cand,
                              Nanos) const noexcept {
    return cand[0];
  }
};

// ---------------------------------------------------------------------------
// ECMP: hash the 5-tuple, index the candidate set.
//
// Per-flow and stateless, which is what gives it the two properties it is
// prized for in real fabrics and which the adaptive router must not be allowed
// to take for free: every packet of a flow follows one path, so there is no
// reordering, and no switch has to store anything per flow.
//
// It is also blind. The hash cannot see that the chosen path is congested, and
// two large flows landing on the same uplink stay there for their whole
// lifetime. That is the specific weakness M3 attacks -- and the reason this
// implementation has to be a *good* ECMP for the comparison to mean anything.
// ---------------------------------------------------------------------------
class EcmpPolicy {
 public:
  constexpr EcmpPolicy() noexcept = default;
  constexpr explicit EcmpPolicy(std::uint64_t seed) noexcept : seed_(seed) {}

  [[nodiscard]] PortId select(NodeId at, const FlowKey& key, std::span<const PortId> cand,
                              Nanos) const noexcept {
    if (cand.size() == 1) {
      return cand[0];
    }
    const std::uint64_t h = flow_hash(key, switch_salt(at.value(), seed_));
    return cand[hash_to_index(h, cand.size())];
  }

  [[nodiscard]] constexpr std::uint64_t seed() const noexcept { return seed_; }

 private:
  // Part of the run seed, not a constant: a fixed hash seed could be
  // accidentally lucky or unlucky on a given topology, and that luck would then
  // be baked into every seed of every sweep. Varying it across seeds turns hash
  // luck into ordinary run-to-run variance, which the paired comparison in M4
  // is built to absorb.
  std::uint64_t seed_ = 0;
};

static_assert(RoutingPolicy<StaticFirstPolicy>);
static_assert(RoutingPolicy<EcmpPolicy>);

}  // namespace fabric
