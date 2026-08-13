#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fabric/core/event.hpp"
#include "fabric/core/event_queue.hpp"
#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/fabric.hpp"
#include "fabric/routing/flow_key.hpp"
#include "fabric/routing/policy.hpp"
#include "fabric/routing/route_table.hpp"

namespace fabric {

struct FlowSpec {
  NodeId src{};
  NodeId dst{};
  Bytes size_bytes = 0;
  Nanos start_ns = 0;
  std::uint16_t mtu_bytes = 1500;

  // The transport-layer half of the 5-tuple ECMP hashes on. The workload picks
  // these; two flows between the same pair of hosts differ only here, which is
  // exactly how a real fabric spreads a multi-connection sender.
  std::uint16_t src_port = 0;
  std::uint16_t dst_port = 0;
  std::uint8_t protocol = 6;

  // Offered load as a fraction of NIC line rate, in parts per thousand. 1000 is
  // line rate. Open loop: the source paces to this rate regardless of what the
  // network is doing, which is the honest M1 model -- there is no feedback
  // until the ack-clocked transport lands in M2. It is also the knob M4's load
  // sweep turns.
  std::uint32_t load_permille = 1000;
};

struct FlowResult {
  Nanos start_ns = 0;
  Nanos finish_ns = 0;
  Bytes delivered_bytes = 0;
  std::uint32_t packets_total = 0;
  std::uint32_t packets_delivered = 0;
  std::uint32_t packets_dropped = 0;
  bool complete = false;

  [[nodiscard]] Nanos fct_ns() const noexcept { return finish_ns - start_ns; }
};

struct RunStats {
  Nanos virtual_end_ns = 0;
  std::uint64_t events_processed = 0;
  Bytes bytes_injected = 0;
  Bytes bytes_delivered = 0;
  Bytes bytes_dropped = 0;
  std::uint32_t flows_complete = 0;
  std::uint32_t flows_incomplete = 0;
};

// ---------------------------------------------------------------------------
// The discrete-event core.
//
// Owns the fabric, the routing table, the flow set and the event heap. Move-only:
// a run holds multi-megabyte state and copying it is never what the caller meant.
//
// Packet lifecycle, and the whole of the hot path:
//
//   Inject      -- flow releases packet i into its host NIC, then schedules
//                  packet i+1 one inter-packet period later. Source pacing lives
//                  entirely in this schedule, which is why M2 can swap in ack
//                  clocking without touching anything else.
//   TxComplete  -- an output port finished clocking its in-flight packet; hand
//                  it to the wire (Arrive one propagation delay later) and start
//                  the next queued packet if there is one.
//   Arrive      -- packet reached the far endpoint. Either it is home, or the
//                  routing policy picks one of the equal-cost output ports of
//                  the receiving switch.
//
// Store-and-forward, output-queued, drop-tail. No retransmission yet -- a
// dropped packet leaves its flow permanently incomplete, and run() reports that
// rather than hiding it. Retransmission arrives with the transport model in M2.
// ---------------------------------------------------------------------------
class Simulation {
 public:
  Simulation(Fabric fabric, RouteTable routes);

  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;
  Simulation(Simulation&&) noexcept = default;
  Simulation& operator=(Simulation&&) noexcept = default;
  ~Simulation() = default;

  FlowId add_flow(const FlowSpec& spec);

  // The routing policy is a template parameter, so the per-hop decision is a
  // direct call the compiler can inline -- no virtual dispatch on the hot path.
  // Instantiated explicitly in simulation.cpp for each shipped policy; the CLI
  // string is resolved to a type once, in the harness.
  template <RoutingPolicy Policy>
  RunStats run(const Policy& policy);

  // Single-path M0 behaviour, and what the analytical validation runs on.
  RunStats run();

  [[nodiscard]] std::span<const FlowResult> results() const noexcept { return results_; }
  [[nodiscard]] std::span<const FlowSpec> specs() const noexcept { return specs_; }
  [[nodiscard]] const Fabric& fabric() const noexcept { return fabric_; }
  [[nodiscard]] Nanos now() const noexcept { return now_; }

  // Digest over every flow outcome, in FlowId order. Two runs of the same
  // configuration must produce the same value or the simulator is not
  // deterministic; the determinism test asserts exactly that.
  [[nodiscard]] std::uint64_t result_digest() const noexcept;

 private:
  // Per-flow state the hot path actually touches: the hash key and the routing
  // table column. Kept apart from FlowSpec so a routing decision does not drag
  // in flow size, start time and MTU it has no use for.
  struct FlowRoute {
    FlowKey key{};
    std::uint32_t dst_host = 0;
  };

  void begin_run();
  void end_run();

  void on_inject(const Event& e);
  void on_tx_complete(const Event& e);

  template <RoutingPolicy Policy>
  void on_arrive(const Event& e, const Policy& policy);

  void enqueue(PortId pid, const Packet& pkt);
  void start_tx(PortId pid);

  Fabric fabric_;
  RouteTable routes_;
  EventQueue queue_;

  std::vector<FlowSpec> specs_;
  std::vector<FlowRoute> froutes_;
  std::vector<FlowResult> results_;

  Nanos now_ = 0;
  RunStats stats_{};
};

}  // namespace fabric
