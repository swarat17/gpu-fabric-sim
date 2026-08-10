#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fabric/core/event.hpp"
#include "fabric/core/event_queue.hpp"
#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/fabric.hpp"
#include "fabric/routing/route_table.hpp"

namespace fabric {

struct FlowSpec {
  NodeId src{};
  NodeId dst{};
  Bytes size_bytes = 0;
  Nanos start_ns = 0;
  std::uint16_t mtu_bytes = 1500;
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
// M0 packet lifecycle, and the whole of the hot path:
//
//   Inject      -- flow releases packet i into its host NIC, then schedules
//                  packet i+1 one serialisation time later. Source pacing lives
//                  entirely in this schedule, which is why M2 can swap in ack
//                  clocking without touching anything else.
//   TxComplete  -- an output port finished clocking its in-flight packet; hand
//                  it to the wire (Arrive one propagation delay later) and start
//                  the next queued packet if there is one.
//   Arrive      -- packet reached the far endpoint. Either it is home, or it is
//                  routed onto an output port of the receiving switch.
//
// Store-and-forward, output-queued, drop-tail. No retransmission in M0 -- a
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

  RunStats run();

  [[nodiscard]] std::span<const FlowResult> results() const noexcept { return results_; }
  [[nodiscard]] const Fabric& fabric() const noexcept { return fabric_; }
  [[nodiscard]] Nanos now() const noexcept { return now_; }

  // Digest over every flow outcome, in FlowId order. Two runs of the same
  // configuration must produce the same value or the simulator is not
  // deterministic; the determinism test asserts exactly that.
  [[nodiscard]] std::uint64_t result_digest() const noexcept;

 private:
  void on_inject(const Event& e);
  void on_tx_complete(const Event& e);
  void on_arrive(const Event& e);

  void enqueue(PortId pid, const Packet& pkt);
  void start_tx(PortId pid);

  [[nodiscard]] PortId select_output(NodeId at, const FlowSpec& spec) const noexcept;

  Fabric fabric_;
  RouteTable routes_;
  EventQueue queue_;

  std::vector<FlowSpec> specs_;
  std::vector<FlowResult> results_;

  Nanos now_ = 0;
  RunStats stats_{};
};

}  // namespace fabric
