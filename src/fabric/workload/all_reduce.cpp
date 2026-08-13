#include "fabric/workload/all_reduce.hpp"

#include <cassert>

#include "fabric/topology/fat_tree.hpp"
#include "fabric/workload/rng.hpp"

namespace fabric {

namespace {

std::vector<std::uint32_t> place_ranks([[maybe_unused]] const Fabric& f,
                                       const AllReduceParams& p, std::uint32_t gpus) {
  std::vector<std::uint32_t> host(gpus, 0);

  if (p.placement == RingPlacement::Sequential) {
    for (std::uint32_t r = 0; r < gpus; ++r) {
      host[r] = r;
    }
    return host;
  }

  const std::uint32_t k = p.fat_tree_k;
  assert(k >= 2 && k % 2 == 0 && "RoundRobinPods needs the fat-tree arity");
  assert(gpus == f.host_count() && "RoundRobinPods places one rank on every host");
  const std::uint32_t per_pod = fat_tree_hosts_per_pod(k);
  for (std::uint32_t r = 0; r < gpus; ++r) {
    host[r] = (r % k) * per_pod + (r / k);
  }
  return host;
}

}  // namespace

AllReduce make_ring_all_reduce(const Fabric& f, const AllReduceParams& params) {
  const std::uint32_t gpus =
      params.gpu_count == 0 ? static_cast<std::uint32_t>(f.host_count()) : params.gpu_count;
  assert(gpus >= 2 && gpus <= f.host_count());

  AllReduce ar;
  ar.gpus = gpus;
  ar.steps = 2 * (gpus - 1);
  ar.chunk_bytes = params.buffer_bytes / gpus;
  ar.chunk_packets = static_cast<std::uint32_t>((ar.chunk_bytes + params.mtu_bytes - 1) /
                                                params.mtu_bytes);
  ar.chunk_exact = (params.buffer_bytes % gpus == 0) && (ar.chunk_bytes % params.mtu_bytes == 0);
  assert(ar.chunk_bytes > 0);

  ar.rank_to_host = place_ranks(f, params, gpus);

  // One source port per rank, not per step. A real job holds a connection open
  // to its ring neighbour for the whole collective, so ECMP hashes the same
  // 5-tuple in every step and an unlucky path stays unlucky for all 2(N-1) of
  // them. Re-drawing the port each step would quietly average that away.
  RandomStream port_rng(params.seed, kStreamPorts);
  std::vector<std::uint16_t> src_port(gpus, 0);
  for (std::uint32_t r = 0; r < gpus; ++r) {
    src_port[r] = static_cast<std::uint16_t>(1024u + port_rng.below(63488u));
  }

  ar.flows.reserve(static_cast<std::size_t>(ar.steps) * gpus);
  for (std::uint32_t s = 0; s < ar.steps; ++s) {
    for (std::uint32_t r = 0; r < gpus; ++r) {
      FlowSpec spec;
      spec.src = f.host_at(ar.rank_to_host[r]);
      spec.dst = f.host_at(ar.rank_to_host[(r + 1) % gpus]);
      spec.size_bytes = ar.chunk_bytes;
      spec.mtu_bytes = params.mtu_bytes;
      spec.window_pkts = params.window_pkts;
      spec.load_permille = params.load_permille;
      spec.src_port = src_port[r];
      spec.dst_port = 5001;
      spec.start_ns = 0;

      if (s > 0) {
        // Rank r can send its step-s chunk only once it has received the step
        // s-1 chunk, which rank r-1 sent.
        const std::uint32_t upstream = (r + gpus - 1) % gpus;
        spec.depends_on = FlowId{(s - 1) * gpus + upstream};
      }
      ar.flows.push_back(spec);
    }
  }

  return ar;
}

}  // namespace fabric
