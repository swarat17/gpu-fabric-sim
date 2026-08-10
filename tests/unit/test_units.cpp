#include <doctest.h>

#include "fabric/core/units.hpp"

using namespace fabric;

TEST_CASE("serialisation time is exact for whole-Gb/s rates at a 1500 B MTU") {
  CHECK(serialization_ns(1500, gbps(100)) == 120);
  CHECK(serialization_ns(1500, gbps(25)) == 480);
  CHECK(serialization_ns(1500, gbps(10)) == 1200);
  CHECK(serialization_is_exact(1500, gbps(100)));
  CHECK(serialization_is_exact(1500, gbps(25)));
}

TEST_CASE("the exactness predicate catches truncating parameter choices") {
  // 1000 B at 3 Gb/s is 2666.67 ns. The model truncates, so any analytical
  // comparison against this configuration would be checking the simulator
  // against a rounded number and calling the agreement a result.
  CHECK_FALSE(serialization_is_exact(1000, gbps(3)));
  CHECK(serialization_ns(1000, gbps(3)) == 2666);
}

TEST_CASE("the largest permitted packet cannot overflow the numerator") {
  constexpr Bytes kNumerator = kMaxPacketBytes * kBitsPerByte * kNanosPerSec;
  static_assert(kNumerator < (~Bytes{0}) / 2,
                "serialization_ns must keep a full order of magnitude of headroom");
  CHECK(kNumerator > 0);
}
