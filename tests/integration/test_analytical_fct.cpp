#include <doctest.h>

#include "fabric/core/units.hpp"
#include "fabric/harness/scenario.hpp"
#include "fabric/topology/leaf_spine.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// The correctness anchor for the whole project.
//
// A single flow across an uncongested, homogeneous path has a closed-form
// completion time (see scenario.hpp). Because the core is integer-only, the
// comparison is an exact equality rather than a tolerance -- if the link model,
// the store-and-forward pipeline or the event ordering is wrong by a single
// nanosecond, this fails. Nearly every modelling bug worth catching shows up
// here first.
// ---------------------------------------------------------------------------

TEST_CASE("a single uncongested flow matches the closed form exactly") {
  const SmokeConfig cfg;
  SmokeScenario sc = build_smoke(cfg);

  REQUIRE_MESSAGE(sc.analytical_exact,
                  "scenario parameters must make the closed form exact, "
                  "otherwise this test compares against a rounded number");

  const RunStats stats = sc.sim.run();
  const FlowResult& r = sc.sim.results()[sc.flow.index()];

  REQUIRE(r.complete);
  CHECK(r.fct_ns() == sc.analytical_ns);
  CHECK(r.packets_delivered == sc.packets);
  CHECK(stats.bytes_delivered == cfg.flow_bytes);
  CHECK(stats.bytes_injected == cfg.flow_bytes);
  CHECK(stats.bytes_dropped == 0);
  CHECK(stats.flows_incomplete == 0);
}

TEST_CASE("the closed form tracks flow size and propagation delay") {
  for (const Bytes size : {Bytes{1500}, Bytes{15'000}, Bytes{150'000}, Bytes{1'500'000}}) {
    for (const Nanos prop : {Nanos{0}, Nanos{100}, Nanos{5'000}}) {
      SmokeConfig cfg;
      cfg.flow_bytes = size;
      cfg.topology.host_link.prop_delay_ns = prop;
      cfg.topology.fabric_link.prop_delay_ns = prop;

      SmokeScenario sc = build_smoke(cfg);
      REQUIRE(sc.analytical_exact);
      sc.sim.run();

      CHECK(sc.sim.results()[sc.flow.index()].fct_ns() == sc.analytical_ns);
    }
  }
}

TEST_CASE("the closed form tracks link rate") {
  for (const BitsPerSec rate : {gbps(10), gbps(25), gbps(100), gbps(400)}) {
    SmokeConfig cfg;
    cfg.topology.host_link.rate = rate;
    cfg.topology.fabric_link.rate = rate;

    SmokeScenario sc = build_smoke(cfg);
    REQUIRE(sc.analytical_exact);
    sc.sim.run();

    CHECK(sc.sim.results()[sc.flow.index()].fct_ns() == sc.analytical_ns);
  }
}

TEST_CASE("a same-leaf flow takes the two-hop path and still matches") {
  SmokeConfig cfg;
  cfg.dst_host = 1;  // same leaf as host 0

  SmokeScenario sc = build_smoke(cfg);
  CHECK(sc.hops == kLeafSpineSameLeafHops);
  REQUIRE(sc.analytical_exact);

  sc.sim.run();
  CHECK(sc.sim.results()[sc.flow.index()].fct_ns() == sc.analytical_ns);
}

TEST_CASE("bytes are conserved end to end") {
  SmokeConfig cfg;
  SmokeScenario sc = build_smoke(cfg);
  const RunStats stats = sc.sim.run();

  // Nothing is in flight once the queue drains, so every injected byte must
  // have been either delivered or dropped.
  CHECK(stats.bytes_injected == stats.bytes_delivered + stats.bytes_dropped);
}
