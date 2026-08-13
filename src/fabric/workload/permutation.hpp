#pragma once

#include <cstdint>
#include <vector>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/fabric.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Permutation traffic: every host sends exactly one flow, every host receives
// exactly one flow.
//
// This is the standard stress test for a Clos fabric, and it is the right first
// workload because it isolates the routing question. A permutation is by
// construction feasible at full line rate -- no endpoint is oversubscribed, and
// a fat-tree has the bisection bandwidth to carry it -- so *any* congestion
// observed is caused by path selection and nothing else. If ECMP builds queues
// here, it is because two flows hashed onto the same uplink while another sat
// idle, which is precisely the phenomenon M3 attacks.
// ---------------------------------------------------------------------------
struct PermutationParams {
  std::uint64_t seed = 0;
  Bytes flow_bytes = 1'000'000;
  std::uint16_t mtu_bytes = 1500;
  std::uint32_t load_permille = 1000;  // offered load per source, 1000 = line rate
  std::uint32_t window_pkts = 32;      // sender window; see min_window_packets()
  Nanos max_start_jitter_ns = 0;       // 0 = every flow starts at t = 0
};

// Sattolo's algorithm: a uniformly random *cyclic* permutation, which is a
// derangement by construction -- no host can be handed itself as a destination,
// so the generator needs no retry loop and no special case.
[[nodiscard]] std::vector<FlowSpec> make_permutation(const Fabric& f,
                                                     const PermutationParams& params);

}  // namespace fabric
