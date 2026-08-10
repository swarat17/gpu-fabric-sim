#include <doctest.h>

#include <cstdint>

#include "fabric/harness/scenario.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// Determinism is the property the whole benchmarking methodology rests on: a
// paired ECMP-vs-adaptive comparison is only meaningful if the two arms really
// did see identical inputs, and a published number is only reproducible if the
// same configuration yields the same answer on someone else's machine.
// ---------------------------------------------------------------------------

TEST_CASE("two independent builds of the same scenario agree bit for bit") {
  SmokeScenario a = build_smoke(SmokeConfig{});
  SmokeScenario b = build_smoke(SmokeConfig{});

  const RunStats sa = a.sim.run();
  const RunStats sb = b.sim.run();

  CHECK(a.sim.result_digest() == b.sim.result_digest());
  CHECK(sa.events_processed == sb.events_processed);
  CHECK(sa.virtual_end_ns == sb.virtual_end_ns);
}

TEST_CASE("re-running the same Simulation object reproduces the run exactly") {
  // Also the state-reset test: run() must clear per-port queues, port busy
  // flags, counters and per-flow outcomes, or the second run would inherit
  // residue from the first.
  SmokeScenario sc = build_smoke(SmokeConfig{});

  const RunStats first = sc.sim.run();
  const std::uint64_t digest_first = sc.sim.result_digest();

  const RunStats second = sc.sim.run();
  const std::uint64_t digest_second = sc.sim.result_digest();

  CHECK(digest_first == digest_second);
  CHECK(first.events_processed == second.events_processed);
  CHECK(first.virtual_end_ns == second.virtual_end_ns);
  CHECK(first.bytes_injected == second.bytes_injected);
  CHECK(first.bytes_delivered == second.bytes_delivered);
  CHECK(first.bytes_dropped == second.bytes_dropped);
}

TEST_CASE("the digest actually discriminates between different runs") {
  // A digest that never changes would make the tests above vacuous.
  SmokeScenario a = build_smoke(SmokeConfig{});

  SmokeConfig other;
  other.flow_bytes = 3'000'000;
  SmokeScenario b = build_smoke(other);

  a.sim.run();
  b.sim.run();

  CHECK(a.sim.result_digest() != b.sim.result_digest());
}
