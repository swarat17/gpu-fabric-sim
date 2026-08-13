#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fabric/routing/flow_key.hpp"
#include "fabric/routing/policy.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// The baseline has to be a good ECMP, not a strawman.
//
// A weak hash collides flows onto the same path far more often than chance.
// That would make ECMP look terrible, make the adaptive router look brilliant,
// and the headline number would be measuring the hash function rather than the
// routing idea. So the uniformity of the flow-to-path mapping is asserted here,
// on deliberately structured inputs -- sequential addresses and sequential
// ports are exactly where a weak mixer fails.
//
// Floating point appears in this file. That is allowed: it is a test, not the
// simulation core, and nothing computed here feeds back into a model.
// ---------------------------------------------------------------------------

namespace {

// Chi-square critical values at p = 0.999 for df = n - 1. A uniform hash exceeds
// these once in a thousand runs; the inputs here are fixed, so the test is
// deterministic and either passes forever or fails forever.
double critical_999(std::size_t buckets) {
  switch (buckets) {
    case 2:
      return 10.828;  // df 1
    case 4:
      return 16.266;  // df 3
    case 8:
      return 24.322;  // df 7
    case 16:
      return 37.697;  // df 15
    default:
      return -1.0;
  }
}

double chi_square(const std::vector<std::uint64_t>& counts, std::uint64_t total) {
  const double expected = static_cast<double>(total) / static_cast<double>(counts.size());
  double stat = 0.0;
  for (const std::uint64_t c : counts) {
    const double d = static_cast<double>(c) - expected;
    stat += (d * d) / expected;
  }
  return stat;
}

std::vector<PortId> candidate_ports(std::size_t n) {
  std::vector<PortId> v;
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    v.push_back(PortId{static_cast<std::uint32_t>(i)});
  }
  return v;
}

// A structured flow population: every ordered pair of 64 hosts, with a source
// port that advances with the pair. This is the shape real permutation and
// all-reduce workloads produce.
std::vector<FlowKey> structured_keys() {
  std::vector<FlowKey> keys;
  keys.reserve(64 * 64);
  for (std::uint32_t src = 0; src < 64; ++src) {
    for (std::uint32_t dst = 0; dst < 64; ++dst) {
      if (src == dst) {
        continue;
      }
      FlowKey k;
      k.src_ip = src;
      k.dst_ip = dst;
      k.src_port = static_cast<std::uint16_t>(1024 + ((src * 64 + dst) % 60000));
      k.dst_port = 5001;
      keys.push_back(k);
    }
  }
  return keys;
}

}  // namespace

TEST_CASE("the ECMP hash spreads structured flows uniformly over the candidates") {
  const std::vector<FlowKey> keys = structured_keys();
  const EcmpPolicy policy(0);
  const NodeId at{7};

  for (const std::size_t n : {std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
    const std::vector<PortId> cand = candidate_ports(n);
    std::vector<std::uint64_t> counts(n, 0);
    for (const FlowKey& k : keys) {
      const PortId chosen = policy.select(at, k, cand, 0);
      REQUIRE(chosen.index() < n);
      ++counts[chosen.index()];
    }
    const double stat = chi_square(counts, keys.size());
    CAPTURE(n);
    CAPTURE(stat);
    CHECK(stat < critical_999(n));
  }
}

TEST_CASE("the hash is uniform for every switch salt, not just a lucky one") {
  // A hash that is uniform at one switch and lumpy at another would produce a
  // fabric with a few permanently unlucky nodes.
  const std::vector<FlowKey> keys = structured_keys();
  const std::vector<PortId> cand = candidate_ports(8);
  const EcmpPolicy policy(0x5EED);

  for (std::uint32_t node = 0; node < 32; ++node) {
    std::vector<std::uint64_t> counts(8, 0);
    for (const FlowKey& k : keys) {
      ++counts[policy.select(NodeId{node}, k, cand, 0).index()];
    }
    CAPTURE(node);
    CHECK(chi_square(counts, keys.size()) < critical_999(8));
  }
}

TEST_CASE("different switches do not make correlated choices") {
  // Hash polarization: if every tier hashed identically, two flows that collided
  // at the edge would collide again at the aggregation tier and again at the
  // core, turning one unlucky hash into an end-to-end shared path. Real fabrics
  // salt per device; so does this one. With 8 candidates, two independent
  // switches should agree about 1 time in 8.
  const std::vector<FlowKey> keys = structured_keys();
  const std::vector<PortId> cand = candidate_ports(8);
  const EcmpPolicy policy(0);

  std::uint64_t agree = 0;
  for (const FlowKey& k : keys) {
    if (policy.select(NodeId{3}, k, cand, 0) == policy.select(NodeId{11}, k, cand, 0)) {
      ++agree;
    }
  }
  const double rate = static_cast<double>(agree) / static_cast<double>(keys.size());
  CAPTURE(rate);
  CHECK(rate > 0.10);
  CHECK(rate < 0.15);
}

TEST_CASE("the choice is stable for a given flow and switch") {
  // Per-flow, not per-packet. This is what makes ECMP reordering-free, and it is
  // the property the adaptive router in M3 must not be allowed to break for
  // free.
  const std::vector<PortId> cand = candidate_ports(16);
  const EcmpPolicy policy(42);
  FlowKey k;
  k.src_ip = 11;
  k.dst_ip = 97;
  k.src_port = 40000;

  const PortId first = policy.select(NodeId{5}, k, cand, 0);
  for (std::uint64_t i = 0; i < 1000; ++i) {
    CHECK(policy.select(NodeId{5}, k, cand, i * 1000) == first);
  }
}

TEST_CASE("a single candidate is always chosen and never hashed") {
  const std::vector<PortId> cand = candidate_ports(1);
  const EcmpPolicy policy(7);
  FlowKey k;
  for (std::uint32_t i = 0; i < 100; ++i) {
    k.src_ip = i;
    CHECK(policy.select(NodeId{i}, k, cand, 0) == cand[0]);
  }
}

TEST_CASE("hash_to_index stays inside the candidate set") {
  for (std::size_t n = 1; n <= 64; ++n) {
    for (std::uint64_t i = 0; i < 512; ++i) {
      const std::uint64_t h = mix64(i * 0x9E37'79B9'7F4A'7C15ULL);
      CHECK(hash_to_index(h, n) < n);
    }
  }
  // The extremes must map to the ends rather than wrapping.
  CHECK(hash_to_index(0, 8) == 0);
  CHECK(hash_to_index(0xFFFF'FFFF'FFFF'FFFFULL, 8) == 7);
}

TEST_CASE("every field of the 5-tuple changes the hash") {
  // A hash that ignored the source port would give every flow between the same
  // host pair the same path -- which is a real failure mode of naive
  // implementations that hash addresses only.
  const FlowKey base{1, 2, 3000, 5001, 6};
  const std::uint64_t h = flow_hash(base, 0);

  FlowKey k = base;
  k.src_ip = 2;
  CHECK(flow_hash(k, 0) != h);
  k = base;
  k.dst_ip = 3;
  CHECK(flow_hash(k, 0) != h);
  k = base;
  k.src_port = 3001;
  CHECK(flow_hash(k, 0) != h);
  k = base;
  k.dst_port = 5002;
  CHECK(flow_hash(k, 0) != h);
  k = base;
  k.protocol = 17;
  CHECK(flow_hash(k, 0) != h);
  CHECK(flow_hash(base, 1) != h);
}
