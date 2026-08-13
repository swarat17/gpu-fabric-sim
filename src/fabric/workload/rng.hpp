#pragma once

#include <cstdint>
#include <random>

namespace fabric {

// ---------------------------------------------------------------------------
// A named random stream.
//
// Two determinism rules are enforced by this type existing at all:
//
//  1. **One stream per concern.** Workload placement, port numbers and (later)
//     tenant traffic each draw from their own generator, seeded by name. If they
//     shared one, adding a single draw to the workload would shift every
//     subsequent number and silently change the traffic in an unrelated part of
//     the scenario -- which would make a paired ECMP-vs-adaptive comparison
//     compare two different experiments.
//
//  2. **No std::uniform_int_distribution.** mt19937_64 is specified bit-exactly
//     by the standard; the distributions are not, and libstdc++ and libc++
//     legitimately produce different values from the same engine. Since the
//     project claims reproducibility on someone else's machine, the bounded
//     draw is implemented here instead.
// ---------------------------------------------------------------------------
class RandomStream {
 public:
  // `name` is a tag mixed into the seed so two streams of the same run never
  // coincide.
  RandomStream(std::uint64_t seed, std::uint64_t name) noexcept
      : gen_(splitmix(seed ^ splitmix(name))) {}

  [[nodiscard]] std::uint64_t next() noexcept { return gen_(); }

  // Uniform on [0, n), unbiased by rejection. n must be positive.
  [[nodiscard]] std::uint32_t below(std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(below64(n));
  }

  [[nodiscard]] std::uint64_t below64(std::uint64_t n) noexcept {
    if (n <= 1) {
      return 0;
    }
    // 2^64 mod n. Draws under this would make the modulo fold non-uniformly.
    const std::uint64_t reject_below = (0ULL - n) % n;
    std::uint64_t r = next();
    while (r < reject_below) {
      r = next();
    }
    return r % n;
  }

 private:
  [[nodiscard]] static constexpr std::uint64_t splitmix(std::uint64_t x) noexcept {
    x += 0x9E37'79B9'7F4A'7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBULL;
    return x ^ (x >> 31);
  }

  std::mt19937_64 gen_;
};

// Stream names. Adding one here never perturbs the existing streams.
inline constexpr std::uint64_t kStreamPermutation = 1;
inline constexpr std::uint64_t kStreamPorts = 2;
inline constexpr std::uint64_t kStreamStartTimes = 3;
inline constexpr std::uint64_t kStreamTenant = 4;  // reserved for M3

}  // namespace fabric
