#pragma once

#include <cstdint>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"
#include "fabric/topology/leaf_spine.hpp"

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

}  // namespace fabric
