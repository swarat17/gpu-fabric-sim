#include "fabric/core/simulation.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace fabric {

Simulation::Simulation(Fabric fabric, RouteTable routes)
    : fabric_(std::move(fabric)), routes_(std::move(routes)) {}

FlowId Simulation::add_flow(const FlowSpec& spec) {
  assert(spec.mtu_bytes > 0 && spec.mtu_bytes <= kMaxPacketBytes);
  assert(spec.size_bytes > 0);
  assert(fabric_.node(spec.src).is_host && fabric_.node(spec.dst).is_host);
  assert(spec.src != spec.dst);
  assert(spec.load_permille > 0 && spec.load_permille <= 1000);
  // A dependency may only name an earlier flow. That makes the dependency graph
  // acyclic by construction, so no run can deadlock waiting on itself.
  assert(!spec.depends_on.valid() || spec.depends_on.index() < specs_.size());

  const auto id = static_cast<std::uint32_t>(specs_.size());
  specs_.push_back(spec);

  const Bytes mtu = spec.mtu_bytes;
  const auto packets_total = static_cast<std::uint32_t>((spec.size_bytes + mtu - 1) / mtu);

  // Host dense indices stand in for IP addresses. splitmix64 mixes small
  // sequential integers as well as it mixes anything else -- that is what it was
  // designed for -- so nothing is gained by dressing them up as dotted quads.
  FlowRoute fr;
  fr.src_host = fabric_.host_index(spec.src);
  fr.dst_host = fabric_.host_index(spec.dst);
  fr.fwd_key = FlowKey{fr.src_host, fr.dst_host, spec.src_port, spec.dst_port, spec.protocol};
  // The ack's 5-tuple is the data tuple reversed, so the reverse path is hashed
  // independently and need not mirror the forward path. Real fabrics behave the
  // same way, and pretending otherwise would make ack delivery suspiciously
  // correlated with data delivery.
  fr.rev_key = FlowKey{fr.dst_host, fr.src_host, spec.dst_port, spec.src_port, spec.protocol};
  fr.src_nic = fabric_.ports_of(spec.src)[0];
  fr.dst_nic = fabric_.ports_of(spec.dst)[0];
  froutes_.push_back(fr);

  FlowTx tx;
  tx.packets_total = packets_total;
  tx.window = std::min(spec.window_pkts, packets_total);
  if (tx.window == 0) {
    tx.window = 1;
  }
  tx_.push_back(tx);

  flow_pkt_offset_.push_back(static_cast<std::uint32_t>(pkt_state_.size()));
  pkt_state_.resize(pkt_state_.size() + packets_total, 0);
  pkt_attempt_.resize(pkt_attempt_.size() + packets_total, 0);

  // A packet can only await retransmission if it was in flight, and moving it
  // there decrements in_flight, so retx_count + in_flight never exceeds the
  // window. Sizing the ring at the window is therefore exact, not a guess.
  retx_offset_.push_back(static_cast<std::uint32_t>(retx_ring_.size()));
  retx_ring_.resize(retx_ring_.size() + tx.window, 0);

  FlowResult r;
  r.start_ns = spec.start_ns;
  r.packets_total = packets_total;
  results_.push_back(r);

  return FlowId{id};
}

void Simulation::build_dependents() {
  const std::size_t n = specs_.size();
  dependents_offset_.assign(n + 1, 0);

  for (const FlowSpec& s : specs_) {
    if (s.depends_on.valid()) {
      ++dependents_offset_[s.depends_on.index() + 1];
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    dependents_offset_[i + 1] += dependents_offset_[i];
  }

  dependents_.assign(dependents_offset_[n], FlowId{});
  std::vector<std::uint32_t> cursor(dependents_offset_.begin(), dependents_offset_.end() - 1);
  for (std::size_t i = 0; i < n; ++i) {
    const FlowSpec& s = specs_[i];
    if (s.depends_on.valid()) {
      dependents_[cursor[s.depends_on.index()]++] = FlowId{static_cast<std::uint32_t>(i)};
    }
  }
}

void Simulation::begin_run() {
  assert(transport_.rto_ns > 0 && "the harness must set a retransmission timeout");
  assert(transport_.max_attempts > 0);
  assert(transport_.ack_bytes > 0 && transport_.ack_bytes <= kMaxPacketBytes);

  queue_.clear();
  fabric_.reset_state();
  now_ = 0;
  stats_ = RunStats{};

  std::fill(pkt_state_.begin(), pkt_state_.end(), std::uint8_t{0});
  std::fill(pkt_attempt_.begin(), pkt_attempt_.end(), std::uint8_t{0});
  build_dependents();

  // A run is repeatable: re-running the same Simulation object must produce the
  // same answer, so per-flow state is cleared here rather than assumed fresh
  // from add_flow().
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    const FlowSpec& spec = specs_[i];

    FlowTx& tx = tx_[i];
    tx.next_seq = 0;
    tx.in_flight = 0;
    tx.acked_count = 0;
    tx.retx_head = 0;
    tx.retx_count = 0;
    tx.next_send_ns = 0;
    tx.send_scheduled = false;
    tx.failed = false;
    tx.period_ns = (serialization_ns(spec.mtu_bytes, fabric_.port(froutes_[i].src_nic).rate) *
                    1000ULL) /
                   spec.load_permille;

    FlowResult& r = results_[i];
    r.start_ns = spec.start_ns;
    r.finish_ns = 0;
    r.delivered_bytes = 0;
    r.packets_delivered = 0;
    r.packets_dropped = 0;
    r.packets_retransmitted = 0;
    r.complete = false;
    r.failed = false;

    // Dependent flows are released by the flow they wait on, not by the clock.
    if (!spec.depends_on.valid()) {
      queue_.push(Event::inject(spec.start_ns, FlowId{static_cast<std::uint32_t>(i)}));
    }
  }
}

void Simulation::end_run() {
  stats_.virtual_end_ns = now_;
  for (const FlowResult& r : results_) {
    if (r.complete) {
      ++stats_.flows_complete;
    } else {
      ++stats_.flows_incomplete;
    }
    if (r.failed) {
      ++stats_.flows_failed;
    }
  }
}

template <RoutingPolicy Policy>
RunStats Simulation::run(const Policy& policy) {
  begin_run();

  // --- hot loop -----------------------------------------------------------
  while (!queue_.empty()) {
    const Event e = queue_.pop();
    assert(e.time_ns >= now_ && "virtual time must be non-decreasing");
    now_ = e.time_ns;
    ++stats_.events_processed;

    switch (e.type) {
      case EventType::Inject:
        on_inject(e);
        break;
      case EventType::TxComplete:
        on_tx_complete(e);
        break;
      case EventType::Arrive:
        on_arrive(e, policy);
        break;
      case EventType::Timeout:
        on_timeout(e);
        break;
    }
  }
  // ------------------------------------------------------------------------

  end_run();
  return stats_;
}

RunStats Simulation::run() { return run(StaticFirstPolicy{}); }

void Simulation::on_inject(const Event& e) {
  const FlowId f = e.flow();
  tx_[f.index()].send_scheduled = false;
  try_send(f);
}

void Simulation::try_send(FlowId f) {
  FlowTx& tx = tx_[f.index()];
  if (tx.failed) {
    return;
  }
  const std::uint32_t base = flow_pkt_offset_[f.index()];
  const std::uint32_t rbase = retx_offset_[f.index()];

  while (tx.in_flight < tx.window) {
    // Pacing gate first, so no packet index is consumed and then put back.
    if (now_ < tx.next_send_ns) {
      if (!tx.send_scheduled) {
        tx.send_scheduled = true;
        queue_.push(Event::inject(tx.next_send_ns, f));
      }
      return;
    }

    std::uint32_t idx = 0;
    bool have = false;

    // Retransmissions go first. An entry whose ack turned up after the timer
    // fired is simply dropped from the ring here.
    while (tx.retx_count > 0) {
      idx = retx_ring_[rbase + tx.retx_head];
      tx.retx_head = (tx.retx_head + 1 == tx.window) ? 0 : tx.retx_head + 1;
      --tx.retx_count;
      if ((pkt_state_[base + idx] & kAcked) == 0) {
        have = true;
        break;
      }
    }
    if (!have) {
      if (tx.next_seq >= tx.packets_total) {
        return;  // everything sent; the window drains on acks
      }
      idx = tx.next_seq++;
    }

    send_packet(f, idx);
  }
}

void Simulation::send_packet(FlowId f, std::uint32_t idx) {
  FlowTx& tx = tx_[f.index()];
  const FlowSpec& spec = specs_[f.index()];
  const std::uint32_t base = flow_pkt_offset_[f.index()];

  const Bytes mtu = spec.mtu_bytes;
  const Bytes offset = static_cast<Bytes>(idx) * mtu;
  const Bytes remaining = spec.size_bytes - offset;
  const auto bytes = static_cast<std::uint16_t>(remaining < mtu ? remaining : mtu);

  const std::uint8_t attempt = pkt_attempt_[base + idx];
  if (attempt > 0) {
    ++stats_.packets_retransmitted;
    ++results_[f.index()].packets_retransmitted;
  }

  pkt_state_[base + idx] = static_cast<std::uint8_t>(pkt_state_[base + idx] | kInFlight);
  ++tx.in_flight;
  stats_.bytes_injected += bytes;
  tx.next_send_ns = now_ + tx.period_ns;

  queue_.push(Event::timeout(now_ + transport_.rto_ns, f, idx, attempt));
  enqueue(froutes_[f.index()].src_nic, Packet{f, idx, bytes, PacketKind::Data, attempt});
}

void Simulation::on_ack(FlowId f, std::uint32_t idx) {
  const std::uint32_t base = flow_pkt_offset_[f.index()];
  std::uint8_t& st = pkt_state_[base + idx];
  if ((st & kAcked) != 0) {
    return;  // duplicate ack
  }
  st = static_cast<std::uint8_t>(st | kAcked);

  FlowTx& tx = tx_[f.index()];
  ++tx.acked_count;
  if ((st & kInFlight) != 0) {
    st = static_cast<std::uint8_t>(st & ~kInFlight);
    --tx.in_flight;
  }
  try_send(f);
}

void Simulation::on_timeout(const Event& e) {
  const FlowId f = e.flow();
  const std::uint32_t idx = e.packet_index();
  const std::uint32_t base = flow_pkt_offset_[f.index()];
  std::uint8_t& st = pkt_state_[base + idx];

  if ((st & kAcked) != 0) {
    return;  // acked before the timer fired
  }
  if (pkt_attempt_[base + idx] != e.attempt()) {
    return;  // a later attempt owns this packet; this timer is stale
  }

  FlowTx& tx = tx_[f.index()];
  if (tx.failed) {
    return;
  }
  if ((st & kInFlight) != 0) {
    st = static_cast<std::uint8_t>(st & ~kInFlight);
    --tx.in_flight;
  }

  if (static_cast<std::uint32_t>(pkt_attempt_[base + idx]) + 1 >= transport_.max_attempts) {
    // Give up rather than retry forever. The flow is reported as failed and
    // never completes, which is loud by design.
    tx.failed = true;
    results_[f.index()].failed = true;
    return;
  }
  ++pkt_attempt_[base + idx];

  const std::uint32_t rbase = retx_offset_[f.index()];
  std::uint32_t tail = tx.retx_head + tx.retx_count;
  if (tail >= tx.window) {
    tail -= tx.window;
  }
  retx_ring_[rbase + tail] = idx;
  ++tx.retx_count;

  try_send(f);
}

void Simulation::on_data_arrival(FlowId f, std::uint32_t idx, std::uint16_t bytes) {
  const std::uint32_t base = flow_pkt_offset_[f.index()];
  std::uint8_t& st = pkt_state_[base + idx];

  stats_.bytes_delivered += bytes;
  if ((st & kDelivered) == 0) {
    st = static_cast<std::uint8_t>(st | kDelivered);
    FlowResult& r = results_[f.index()];
    ++r.packets_delivered;
    r.delivered_bytes += bytes;
    if (r.packets_delivered == r.packets_total) {
      complete_flow(f);
    }
  }

  // Ack even a duplicate. A duplicate exists because an ack was lost or a timer
  // fired early, and only another ack can tell the sender otherwise.
  const auto ack_bytes = static_cast<std::uint16_t>(transport_.ack_bytes);
  stats_.ack_bytes_injected += ack_bytes;
  enqueue(froutes_[f.index()].dst_nic, Packet{f, idx, ack_bytes, PacketKind::Ack, 0});
}

void Simulation::complete_flow(FlowId f) {
  FlowResult& r = results_[f.index()];
  r.finish_ns = now_;
  r.complete = true;

  const std::uint32_t begin = dependents_offset_[f.index()];
  const std::uint32_t end = dependents_offset_[f.index() + 1];
  for (std::uint32_t i = begin; i < end; ++i) {
    release_flow(dependents_[i]);
  }
}

void Simulation::release_flow(FlowId d) {
  // The dependent's completion-time clock starts now, not at its nominal
  // start_ns: a collective step that spent its life waiting on the previous
  // step did not take that long to transfer.
  results_[d.index()].start_ns = now_;
  tx_[d.index()].next_send_ns = now_;
  queue_.push(Event::inject(now_, d));
}

void Simulation::on_tx_complete(const Event& e) {
  const PortId pid = e.port();
  Port& p = fabric_.port(pid);
  const Packet pkt = p.in_flight;

  ++p.packets_sent;
  p.bytes_sent += pkt.bytes;

  queue_.push(Event::arrive(now_ + p.prop_delay_ns, pid, pkt));

  if (!p.queue.empty()) {
    start_tx(pid);
  } else {
    p.busy = false;
  }
}

template <RoutingPolicy Policy>
void Simulation::on_arrive(const Event& e, const Policy& policy) {
  const NodeId at = fabric_.port(e.port()).peer_node;
  const FlowId f = e.flow();
  const bool is_ack = e.kind == PacketKind::Ack;

  // Routing only ever forwards toward the destination and a host is never a
  // transit node, so arriving at a host means arriving home -- at the receiver
  // for data, back at the sender for an ack.
  if (fabric_.node(at).is_host) {
    if (is_ack) {
      assert(at == specs_[f.index()].src);
      stats_.ack_bytes_delivered += e.bytes;
      on_ack(f, e.packet_index());
    } else {
      assert(at == specs_[f.index()].dst);
      on_data_arrival(f, e.packet_index(), e.bytes);
    }
    return;
  }

  const FlowRoute& fr = froutes_[f.index()];
  const std::uint32_t dst_host = is_ack ? fr.src_host : fr.dst_host;
  const FlowKey& key = is_ack ? fr.rev_key : fr.fwd_key;

  const std::span<const PortId> cand = routes_.candidates(at, dst_host);
  assert(!cand.empty() && "no route to destination");

  enqueue(policy.select(at, key, cand, now_),
          Packet{f, e.packet_index(), e.bytes, e.kind, 0});
}

void Simulation::enqueue(PortId pid, const Packet& pkt) {
  Port& p = fabric_.port(pid);

  if (!p.queue.try_push(pkt)) {
    // Drop-tail. The sender is told nothing; its retransmission timer is the
    // only thing that will notice.
    ++p.packets_dropped;
    p.bytes_dropped += pkt.bytes;
    if (pkt.kind == PacketKind::Ack) {
      stats_.ack_bytes_dropped += pkt.bytes;
    } else {
      stats_.bytes_dropped += pkt.bytes;
      ++results_[pkt.flow.index()].packets_dropped;
    }
    return;
  }

  if (!p.busy) {
    start_tx(pid);
  }
}

void Simulation::start_tx(PortId pid) {
  Port& p = fabric_.port(pid);
  p.in_flight = p.queue.pop();
  p.busy = true;
  queue_.push(Event::tx_complete(now_ + serialization_ns(p.in_flight.bytes, p.rate), pid));
}

std::uint64_t Simulation::result_digest() const noexcept {
  std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
  for (const FlowResult& r : results_) {
    const std::uint64_t packed = (static_cast<std::uint64_t>(r.packets_delivered) << 32) |
                                 static_cast<std::uint64_t>(r.packets_dropped);
    const std::uint64_t flags = (static_cast<std::uint64_t>(r.packets_retransmitted) << 2) |
                                (static_cast<std::uint64_t>(r.failed) << 1) |
                                static_cast<std::uint64_t>(r.complete);
    h = mix64(h ^ r.start_ns);
    h = mix64(h ^ r.finish_ns);
    h = mix64(h ^ r.delivered_bytes);
    h = mix64(h ^ packed);
    h = mix64(h ^ flags);
  }
  return h;
}

// Every policy the binary ships, instantiated at the end of the translation
// unit where all the templates it calls are defined. This is what keeps the hot
// loop out of every header that mentions a Simulation; M3 adds one more line.
template RunStats Simulation::run<StaticFirstPolicy>(const StaticFirstPolicy&);
template RunStats Simulation::run<EcmpPolicy>(const EcmpPolicy&);

}  // namespace fabric
