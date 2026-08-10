#pragma once

#include <cstdint>

namespace fabric {

// ---------------------------------------------------------------------------
// The simulation core is integer-only, deliberately.
//
// Virtual time is uint64 nanoseconds and sizes are uint64 bytes. There is no
// floating point anywhere below harness/ or metrics/. This is what lets the
// project claim bit-identical results for a given seed across compilers and
// optimisation levels -- floating-point contraction, x87 excess precision and
// -ffast-math style reassociation would all quietly break that.
// ---------------------------------------------------------------------------

using Nanos      = std::uint64_t;  // virtual time, nanoseconds
using Bytes      = std::uint64_t;
using BitsPerSec = std::uint64_t;

inline constexpr Nanos kNanosPerSec = 1'000'000'000ULL;
inline constexpr std::uint64_t kBitsPerByte = 8ULL;

// Largest packet the model accepts. Bounded so that Packet::bytes fits in a
// uint16_t and so serialization_ns() cannot overflow (see below).
inline constexpr Bytes kMaxPacketBytes = 9216;

[[nodiscard]] inline constexpr BitsPerSec gbps(std::uint64_t g) noexcept {
  return g * 1'000'000'000ULL;
}
[[nodiscard]] inline constexpr Nanos micros(std::uint64_t us) noexcept { return us * 1'000ULL; }

// Time to clock `bytes` onto a link running at `rate`.
//
// Truncating integer division. Scenarios used for analytical validation must
// choose (bytes, rate) pairs that divide exactly -- with rates expressed in
// whole Gb/s the divisor is a multiple of 1e9, so `bytes * 8` must be a
// multiple of the Gb/s figure. See docs/validation.md.
//
// Overflow: the numerator is bytes * 8 * 1e9. With bytes <= kMaxPacketBytes
// that peaks near 7.4e13, comfortably inside uint64 (~1.8e19). Callers must
// never pass a whole flow size here, only a single packet.
[[nodiscard]] inline constexpr Nanos serialization_ns(Bytes bytes, BitsPerSec rate) noexcept {
  return (bytes * kBitsPerByte * kNanosPerSec) / rate;
}

// True when serialization_ns() is exact rather than truncated. Validation
// scenarios assert this so a rounding artefact can never be mistaken for a
// modelling result.
[[nodiscard]] inline constexpr bool serialization_is_exact(Bytes bytes, BitsPerSec rate) noexcept {
  return (bytes * kBitsPerByte * kNanosPerSec) % rate == 0;
}

}  // namespace fabric
