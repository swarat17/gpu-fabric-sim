#pragma once

#include <cstdint>
#include <vector>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/fabric.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Ring all-reduce -- the workload the headline claim is about.
//
// N GPUs in a logical ring, each holding S bytes of gradients. The collective
// runs in 2(N-1) steps: N-1 of reduce-scatter followed by N-1 of all-gather. In
// every step, rank r sends one S/N chunk to rank r+1 and receives one from rank
// r-1, and it cannot send in step s until the chunk it received in step s-1 has
// arrived.
//
// That last sentence is the whole reason this workload matters. The steps form a
// dependency chain, so the collective is a **barrier**: it finishes when the
// slowest link in the chain finishes, not when the average one does. A routing
// decision that leaves one flow queued behind another does not cost the average
// -- it costs the entire collective. This is why the project reports p99 rather
// than mean FCT, and why "a few flows got unlucky" is not a small problem.
//
// The dependency is expressed with FlowSpec::depends_on, so the simulator core
// needs no notion of a collective at all.
// ---------------------------------------------------------------------------

enum class RingPlacement : std::uint8_t {
  // Rank r on host r. Ring neighbours are mostly under the same edge switch, so
  // most steps never touch the fabric. Cheap, and unrepresentative of a job
  // scheduled across a shared cluster.
  Sequential = 0,
  // Rank r on pod (r mod k), position (r div k). Every ring hop crosses pods, so
  // every step traverses the full six-hop path and uses the equal-cost paths the
  // routing policy is being judged on. This is the headline placement.
  RoundRobinPods = 1,
};

struct AllReduceParams {
  std::uint64_t seed = 0;
  Bytes buffer_bytes = 19'200'000;  // gradient buffer per GPU
  std::uint16_t mtu_bytes = 1500;
  std::uint32_t window_pkts = 32;
  std::uint32_t gpu_count = 0;  // 0 = every host in the fabric
  std::uint32_t load_permille = 1000;
  RingPlacement placement = RingPlacement::RoundRobinPods;
  std::uint32_t fat_tree_k = 0;  // required for RoundRobinPods
};

struct AllReduce {
  // Step-major: flow index s*N + r is rank r's send in step s. The simulator
  // assigns FlowIds in insertion order, so index == FlowId, which is what lets
  // the dependency chain be wired here rather than after the fact.
  std::vector<FlowSpec> flows;
  std::vector<std::uint32_t> rank_to_host;

  std::uint32_t gpus = 0;
  std::uint32_t steps = 0;  // 2(N-1)
  Bytes chunk_bytes = 0;
  std::uint32_t chunk_packets = 0;

  // The buffer splits into N whole chunks of whole packets. Required before the
  // closed form means anything.
  bool chunk_exact = false;
};

[[nodiscard]] AllReduce make_ring_all_reduce(const Fabric& f, const AllReduceParams& params);

}  // namespace fabric
