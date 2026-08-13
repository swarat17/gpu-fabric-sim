#pragma once

#include <cstddef>
#include <cstdint>

namespace fabric {

// ---------------------------------------------------------------------------
// The 5-tuple a real switch hashes on, and the hash itself.
//
// This file is load-bearing for the credibility of the whole project. The
// headline claim is "adaptive routing beats ECMP", so a weak hash would hand
// the adaptive router a free win: a poorly mixing function collides flows onto
// the same path far more often than chance, ECMP looks terrible, and the
// comparison measures nothing but the strawman. tests/unit/test_ecmp_hash.cpp
// runs a chi-square uniformity test precisely so that cannot happen quietly.
// ---------------------------------------------------------------------------

struct FlowKey {
  std::uint32_t src_ip = 0;
  std::uint32_t dst_ip = 0;
  std::uint16_t src_port = 0;
  std::uint16_t dst_port = 0;
  std::uint8_t protocol = 6;  // TCP-ish; constant in this model, hashed anyway
};

static_assert(sizeof(FlowKey) <= 16);

// splitmix64 finalizer: three xor-shift-multiply rounds, full 64-bit avalanche,
// a handful of cycles. Same primitive as the result digest in simulation.cpp.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xBF58'476D'1CE4'E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D0'49BB'1331'11EBULL;
  x ^= x >> 31;
  return x;
}

// Hash of the 5-tuple under a per-switch salt.
//
// The salt models what real multi-tier fabrics do and must do: if every switch
// hashed identically, a flow that collides with another at the edge would
// collide with it again at the aggregation tier and again at the core. That is
// hash polarization, it is a real and well documented failure of naive ECMP,
// and reproducing it here would flatter the adaptive router for a reason that
// has nothing to do with adaptivity. One salt per switch decorrelates the tiers.
[[nodiscard]] constexpr std::uint64_t flow_hash(const FlowKey& k, std::uint64_t salt) noexcept {
  const std::uint64_t addrs =
      (static_cast<std::uint64_t>(k.src_ip) << 32) | static_cast<std::uint64_t>(k.dst_ip);
  const std::uint64_t ports = (static_cast<std::uint64_t>(k.src_port) << 48) |
                              (static_cast<std::uint64_t>(k.dst_port) << 32) |
                              (static_cast<std::uint64_t>(k.protocol) << 24);
  return mix64(mix64(addrs ^ 0x9E37'79B9'7F4A'7C15ULL) ^ ports ^ salt);
}

// Per-switch hash seed. Derived from the configured run seed so the choice is
// reproducible, and varied across switches so the tiers stay decorrelated.
[[nodiscard]] constexpr std::uint64_t switch_salt(std::uint32_t node, std::uint64_t seed) noexcept {
  return mix64(seed ^ ((static_cast<std::uint64_t>(node) << 1) | 1ULL));
}

// Map a hash onto [0, n) without a division.
//
// Lemire's multiply-shift: treat the top 32 bits as a fraction of 2^32 and take
// the integer part of h*n. Bias is bounded by n/2^32 -- for the fan-outs here
// (at most a few dozen) that is under one part in 10^8, far below anything the
// chi-square test could see, and it costs one multiply instead of a divide on
// the hot path.
[[nodiscard]] constexpr std::uint32_t hash_to_index(std::uint64_t h, std::size_t n) noexcept {
  const std::uint64_t frac = h >> 32;
  return static_cast<std::uint32_t>((frac * static_cast<std::uint64_t>(n)) >> 32);
}

}  // namespace fabric
