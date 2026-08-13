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

// ---------------------------------------------------------------------------
// Transport parameters, shared by every flow in a run.
//
// A fixed window with no reactive congestion control: the sender keeps at most
// `window_pkts` packets outstanding and releases the next one when an ack comes
// back. This is deliberately not DCQCN or TIMELY -- see docs/design.md. What it
// buys is the thing M1 did not have: a sender that cannot overrun the network
// indefinitely, and a dropped packet that is retransmitted rather than
// stranding its flow forever.
// ---------------------------------------------------------------------------
struct TransportConfig {
  // Acks are real packets on real links. They queue, they can be dropped, and
  // they consume reverse-path capacity.
  Bytes ack_bytes = 64;
  // Retransmission timeout. There is no RTT estimator and no backoff: the value
  // is set once from the topology by the harness, which keeps it a documented
  // constant of the experiment rather than a hidden control loop that would
  // entangle the routing result with timer tuning.
  Nanos rto_ns = 0;
  // A flow that burns this many attempts on one packet gives up and is reported
  // as failed. Without a cap a pathological run never terminates.
  std::uint8_t max_attempts = 8;
};

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

  // Sender window in packets. Must cover the bandwidth-delay product or the
  // window itself becomes the bottleneck -- see min_window_packets() in
  // harness/scenario.hpp, which the validation scenarios assert.
  std::uint32_t window_pkts = 32;

  // Additional pacing on top of the window, in parts per thousand of NIC line
  // rate. 1000 leaves the window in sole control. This is the knob M4's load
  // sweep turns; a closed-loop sender alone has no notion of offered load.
  std::uint32_t load_permille = 1000;

  // If valid, this flow does not start at start_ns -- it starts when the named
  // flow completes, and its FCT clock starts there too. This is what turns a
  // collective into a barrier instead of a batch of independent transfers.
  FlowId depends_on{};
};

struct FlowResult {
  Nanos start_ns = 0;
  Nanos finish_ns = 0;
  Bytes delivered_bytes = 0;  // unique bytes; duplicates are not counted twice
  std::uint32_t packets_total = 0;
  std::uint32_t packets_delivered = 0;
  std::uint32_t packets_dropped = 0;
  std::uint32_t packets_retransmitted = 0;
  // Completion is a property of the receiver -- it has every byte. Failure is a
  // property of the sender -- it exhausted max_attempts on some packet. They are
  // not opposites: a sender whose acks are being dropped can give up on a packet
  // the receiver already holds, so a flow can be both complete and failed.
  bool complete = false;
  bool failed = false;

  [[nodiscard]] Nanos fct_ns() const noexcept { return finish_ns - start_ns; }
};

struct RunStats {
  Nanos virtual_end_ns = 0;
  std::uint64_t events_processed = 0;

  // Data bytes. Injected counts every transmission attempt and delivered counts
  // every arrival including duplicates, so injected = delivered + dropped holds
  // exactly once the queue drains.
  Bytes bytes_injected = 0;
  Bytes bytes_delivered = 0;
  Bytes bytes_dropped = 0;

  // Acks, accounted separately so the data conservation identity stays readable
  // and so the cost of the feedback channel is visible rather than hidden.
  Bytes ack_bytes_injected = 0;
  Bytes ack_bytes_delivered = 0;
  Bytes ack_bytes_dropped = 0;

  std::uint64_t packets_retransmitted = 0;
  std::uint32_t flows_complete = 0;
  std::uint32_t flows_incomplete = 0;
  std::uint32_t flows_failed = 0;
};

// ---------------------------------------------------------------------------
// The discrete-event core.
//
// Owns the fabric, the routing table, the flow set and the event heap. Move-only:
// a run holds multi-megabyte state and copying it is never what the caller meant.
//
// Packet lifecycle, and the whole of the hot path:
//
//   Inject      -- a flow may send. Fired at flow start, when the flow it
//                  depends on completes, and when a paced send had to wait.
//                  How much actually goes out is decided by the window.
//   TxComplete  -- an output port finished clocking its in-flight packet; hand
//                  it to the wire (Arrive one propagation delay later) and start
//                  the next queued packet if there is one.
//   Arrive      -- a packet reached the far endpoint. At a switch it is routed
//                  onward; at the destination host a data packet is delivered
//                  and acked; at the source host an ack opens the window.
//   Timeout     -- a retransmission timer expired. Ignored if the packet was
//                  acked or if a later attempt has superseded this timer.
//
// Store-and-forward, output-queued, drop-tail, with a fixed-window ack-clocked
// sender above it. Acks traverse the reverse path as real packets and are
// routed on the reversed 5-tuple, so forward and reverse paths need not agree --
// which is what a real fabric does.
// ---------------------------------------------------------------------------
class Simulation {
 public:
  Simulation(Fabric fabric, RouteTable routes);

  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;
  Simulation(Simulation&&) noexcept = default;
  Simulation& operator=(Simulation&&) noexcept = default;
  ~Simulation() = default;

  // Must be set before run(); an unset rto_ns is a hard error rather than a
  // silent default, because a wrong timeout quietly changes every number.
  void set_transport(const TransportConfig& cfg) noexcept { transport_ = cfg; }
  [[nodiscard]] const TransportConfig& transport() const noexcept { return transport_; }

  FlowId add_flow(const FlowSpec& spec);

  // The routing policy is a template parameter, so the per-hop decision is a
  // direct call the compiler can inline -- no virtual dispatch on the hot path.
  // Instantiated explicitly in simulation.cpp for each shipped policy; the CLI
  // string is resolved to a type once, in the harness.
  template <RoutingPolicy Policy>
  RunStats run(const Policy& policy);

  // Single-path behaviour, and what the analytical validation runs on.
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
  // Per-flow routing state the hot path touches: two hash keys and two routing
  // table columns, because an ack is routed independently of its data.
  struct FlowRoute {
    FlowKey fwd_key{};
    FlowKey rev_key{};
    std::uint32_t dst_host = 0;
    std::uint32_t src_host = 0;
    PortId src_nic{};
    PortId dst_nic{};
  };

  // Per-flow sender state.
  struct FlowTx {
    std::uint32_t next_seq = 0;       // lowest packet index never yet sent
    std::uint32_t in_flight = 0;      // sent, neither acked nor timed out
    std::uint32_t window = 0;
    std::uint32_t packets_total = 0;
    std::uint32_t acked_count = 0;
    std::uint32_t retx_head = 0;      // ring of indices awaiting retransmission
    std::uint32_t retx_count = 0;
    Nanos period_ns = 0;              // pacing floor between two sends
    Nanos next_send_ns = 0;
    bool send_scheduled = false;
    bool failed = false;
  };

  // Per-packet bookkeeping, one byte per packet in a flat array indexed through
  // flow_pkt_offset_. Two bits of state and an attempt counter is all the
  // transport needs, and keeping it flat keeps it out of a vector of vectors.
  static constexpr std::uint8_t kDelivered = 1u << 0;  // receiver has it
  static constexpr std::uint8_t kAcked = 1u << 1;      // sender saw an ack
  static constexpr std::uint8_t kInFlight = 1u << 2;   // counted in FlowTx::in_flight

  void begin_run();
  void end_run();
  void build_dependents();

  void on_inject(const Event& e);
  void on_tx_complete(const Event& e);
  void on_timeout(const Event& e);

  template <RoutingPolicy Policy>
  void on_arrive(const Event& e, const Policy& policy);

  void try_send(FlowId f);
  void send_packet(FlowId f, std::uint32_t idx);
  void on_ack(FlowId f, std::uint32_t idx);
  void on_data_arrival(FlowId f, std::uint32_t idx, std::uint16_t bytes);
  void complete_flow(FlowId f);
  void release_flow(FlowId f);

  void enqueue(PortId pid, const Packet& pkt);
  void start_tx(PortId pid);

  Fabric fabric_;
  RouteTable routes_;
  EventQueue queue_;
  TransportConfig transport_{};

  std::vector<FlowSpec> specs_;
  std::vector<FlowRoute> froutes_;
  std::vector<FlowTx> tx_;
  std::vector<FlowResult> results_;

  std::vector<std::uint32_t> flow_pkt_offset_;
  std::vector<std::uint8_t> pkt_state_;
  std::vector<std::uint8_t> pkt_attempt_;

  std::vector<std::uint32_t> retx_offset_;
  std::vector<std::uint32_t> retx_ring_;

  // Flows released by a given flow's completion, CSR-packed. Rebuilt per run.
  std::vector<std::uint32_t> dependents_offset_;
  std::vector<FlowId> dependents_;

  Nanos now_ = 0;
  RunStats stats_{};
};

}  // namespace fabric
