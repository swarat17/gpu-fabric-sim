#include <doctest.h>

#include <cstdint>

#include "fabric/harness/scenario.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// The transport is a fixed window, ack clocked, with a retransmission timer.
//
// The properties worth pinning are the ones a reviewer would probe: that acks
// cost real wire time, that the window really is the throughput limit when it is
// too small, that a dropped packet is recovered rather than stranding its flow,
// and that a hopeless flow gives up instead of running forever.
// ---------------------------------------------------------------------------

namespace {

// Round trip for the default smoke fabric: 100 Gb/s, 100 ns per link.
constexpr Nanos kSer = 120;      // 1500 B at 100 Gb/s
constexpr Nanos kAckSer = 5;     // 64 B at 100 Gb/s, truncated
constexpr Nanos kProp = 100;
constexpr std::uint32_t kHops = 4;

}  // namespace

TEST_CASE("acks do not disturb the closed form when they have the reverse path to themselves") {
  // A directed link is a port, so the ack stream never queues behind the data it
  // acknowledges. With one sender and one receiver that makes the single-flow
  // identity survive the arrival of a real transport untouched.
  SmokeScenario sc = build_smoke(SmokeConfig{});
  REQUIRE(sc.analytical_exact);

  const RunStats stats = sc.sim.run();
  const FlowResult& r = sc.sim.results()[sc.flow.index()];

  CHECK(r.fct_ns() == sc.analytical_ns);
  CHECK(stats.packets_retransmitted == 0);
  CHECK(stats.ack_bytes_injected == static_cast<Bytes>(sc.packets) * 64);
}

TEST_CASE("ack bytes are conserved just like data bytes") {
  SmokeScenario sc = build_smoke(SmokeConfig{});
  const RunStats stats = sc.sim.run();

  CHECK(stats.bytes_injected == stats.bytes_delivered + stats.bytes_dropped);
  CHECK(stats.ack_bytes_injected == stats.ack_bytes_delivered + stats.ack_bytes_dropped);
  CHECK(stats.ack_bytes_dropped == 0);
}

TEST_CASE("a window of one packet throttles the flow to one packet per round trip") {
  // The exact price of an undersized window, asserted rather than assumed: the
  // sender waits a full round trip between packets, so the closed form for the
  // link no longer describes the flow at all.
  SmokeConfig cfg;
  cfg.window_pkts = 1;
  cfg.flow_bytes = 10 * 1500;

  SmokeScenario sc = build_smoke(cfg);
  REQUIRE_FALSE(sc.analytical_exact);  // the harness knows the window throttles

  sc.sim.run();
  const FlowResult& r = sc.sim.results()[sc.flow.index()];

  const Nanos rtt = round_trip_ns(kHops, kSer, kAckSer, kProp);
  const Nanos expected = 9 * rtt + kHops * (kSer + kProp);

  REQUIRE(r.complete);
  CHECK(r.fct_ns() == expected);
  CHECK(r.fct_ns() > sc.analytical_ns);
}

TEST_CASE("the harness sizes the window above the bandwidth-delay product") {
  // Sanity on the guard itself: with a 5 us propagation delay the pipe holds far
  // more packets, and a window that was fine at 100 ns is not.
  const Nanos rtt_short = round_trip_ns(kHops, kSer, kAckSer, kProp);
  const Nanos rtt_long = round_trip_ns(kHops, kSer, kAckSer, 5000);
  CHECK(rtt_long > rtt_short);
  CHECK(min_window_packets(kHops, kSer, kAckSer, 5000) >
        min_window_packets(kHops, kSer, kAckSer, kProp));

  // A flow shorter than the window cannot be throttled by it.
  CHECK(window_is_sufficient(1, 1, kHops, kSer, kAckSer, 5000));
  CHECK_FALSE(window_is_sufficient(1, 1000, kHops, kSer, kAckSer, 5000));
}

namespace {

// A fat-tree with buffers small enough that ECMP collisions really do overflow
// them, which is the only way to exercise retransmission.
PermutationConfig lossy_config(std::uint8_t max_attempts) {
  PermutationConfig cfg;
  cfg.topology.k = 4;
  cfg.topology.host_link.queue_capacity_pkts = 4;
  cfg.topology.fabric_link.queue_capacity_pkts = 4;
  cfg.routing = RoutingAlgorithm::Ecmp;
  cfg.workload.seed = 7;
  cfg.workload.flow_bytes = 60'000;  // 40 packets
  cfg.workload.window_pkts = 16;
  cfg.transport.max_attempts = max_attempts;
  return cfg;
}

}  // namespace

TEST_CASE("a dropped packet is retransmitted and its flow still completes") {
  PermutationScenario sc = build_permutation(lossy_config(8));
  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);

  REQUIRE_MESSAGE(stats.bytes_dropped > 0,
                  "the scenario is supposed to overflow buffers; if it stopped "
                  "dropping, this test no longer tests anything");
  CHECK(stats.packets_retransmitted > 0);
  CHECK(stats.flows_complete == sc.flow_count);
  CHECK(stats.flows_incomplete == 0);
  CHECK(stats.flows_failed == 0);

  // Retransmissions mean more bytes go in than come out uniquely, but every
  // byte still ends up delivered or dropped.
  CHECK(stats.bytes_injected == stats.bytes_delivered + stats.bytes_dropped);
  CHECK(stats.bytes_injected > static_cast<Bytes>(sc.flow_count) * 60'000);

  for (const FlowResult& r : sc.sim.results()) {
    CHECK(r.delivered_bytes == 60'000);  // unique bytes, duplicates not counted
  }
}

TEST_CASE("a flow gives up rather than retrying forever") {
  // One attempt only: the first drop is fatal. What matters is that the run
  // terminates and says so, not that it recovers.
  PermutationScenario sc = build_permutation(lossy_config(1));
  const RunStats stats = run_with_routing(sc.sim, sc.routing, sc.seed);

  CHECK(stats.flows_failed > 0);
  CHECK(stats.flows_incomplete > 0);
  CHECK(stats.packets_retransmitted == 0);

  // Giving up and completing are not opposites. A sender whose *ack* was lost
  // exhausts its attempts on a packet the receiver already has, so a flow can
  // be both complete and failed. Completion is a property of the receiver;
  // failure is a property of the sender.
  std::uint32_t complete_but_failed = 0;
  for (const FlowResult& r : sc.sim.results()) {
    if (r.complete && r.failed) {
      ++complete_but_failed;
    }
  }
  CHECK(stats.flows_incomplete + complete_but_failed >= stats.flows_failed);
}

TEST_CASE("the transport is deterministic under loss") {
  PermutationScenario a = build_permutation(lossy_config(8));
  PermutationScenario b = build_permutation(lossy_config(8));

  const RunStats sa = run_with_routing(a.sim, a.routing, a.seed);
  const RunStats sb = run_with_routing(b.sim, b.routing, b.seed);

  CHECK(a.sim.result_digest() == b.sim.result_digest());
  CHECK(sa.packets_retransmitted == sb.packets_retransmitted);
  CHECK(sa.bytes_dropped == sb.bytes_dropped);
  CHECK(sa.virtual_end_ns == sb.virtual_end_ns);
}
