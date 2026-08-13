#include "fabric/workload/permutation.hpp"

#include <cassert>

#include "fabric/workload/rng.hpp"

namespace fabric {

std::vector<FlowSpec> make_permutation(const Fabric& f, const PermutationParams& params) {
  const auto n = static_cast<std::uint32_t>(f.host_count());
  assert(n >= 2 && "a permutation needs at least two hosts");
  assert(params.load_permille > 0 && params.load_permille <= 1000);

  RandomStream perm_rng(params.seed, kStreamPermutation);
  RandomStream port_rng(params.seed, kStreamPorts);
  RandomStream start_rng(params.seed, kStreamStartTimes);

  std::vector<std::uint32_t> dst(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = i;
  }
  // Sattolo: draw j strictly below i, so no element can stay put.
  for (std::uint32_t i = n - 1; i > 0; --i) {
    const std::uint32_t j = perm_rng.below(i);
    const std::uint32_t tmp = dst[i];
    dst[i] = dst[j];
    dst[j] = tmp;
  }

  std::vector<FlowSpec> flows;
  flows.reserve(n);
  for (std::uint32_t h = 0; h < n; ++h) {
    assert(dst[h] != h);
    FlowSpec s;
    s.src = f.host_at(h);
    s.dst = f.host_at(dst[h]);
    s.size_bytes = params.flow_bytes;
    s.mtu_bytes = params.mtu_bytes;
    s.load_permille = params.load_permille;
    // Ephemeral source port, well-known destination port: the realistic shape,
    // and the source port is where a real sender's hash entropy comes from.
    s.src_port = static_cast<std::uint16_t>(1024u + port_rng.below(63488u));
    s.dst_port = 5001;
    s.start_ns = params.max_start_jitter_ns == 0
                     ? Nanos{0}
                     : start_rng.below64(params.max_start_jitter_ns + 1);
    flows.push_back(s);
  }
  return flows;
}

}  // namespace fabric
