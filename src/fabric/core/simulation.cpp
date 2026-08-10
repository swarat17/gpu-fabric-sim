#include "fabric/core/simulation.hpp"

#include <cassert>
#include <utility>

namespace fabric {

namespace {

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
  x += 0x9E37'79B9'7F4A'7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBULL;
  return x ^ (x >> 31);
}

}  // namespace

Simulation::Simulation(Fabric fabric, RouteTable routes)
    : fabric_(std::move(fabric)), routes_(std::move(routes)) {}

FlowId Simulation::add_flow(const FlowSpec& spec) {
  assert(spec.mtu_bytes > 0 && spec.mtu_bytes <= kMaxPacketBytes);
  assert(spec.size_bytes > 0);
  assert(fabric_.node(spec.src).is_host && fabric_.node(spec.dst).is_host);
  assert(spec.src != spec.dst);

  const auto id = static_cast<std::uint32_t>(specs_.size());
  specs_.push_back(spec);

  FlowResult r;
  r.start_ns = spec.start_ns;
  const Bytes mtu = spec.mtu_bytes;
  r.packets_total = static_cast<std::uint32_t>((spec.size_bytes + mtu - 1) / mtu);
  results_.push_back(r);

  return FlowId{id};
}

RunStats Simulation::run() {
  queue_.clear();
  fabric_.reset_state();
  now_ = 0;
  stats_ = RunStats{};

  // A run is repeatable: re-running the same Simulation object must produce the
  // same answer, so per-flow outcome state is cleared here rather than assumed
  // fresh from add_flow().
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    FlowResult& r = results_[i];
    r.finish_ns = 0;
    r.delivered_bytes = 0;
    r.packets_delivered = 0;
    r.packets_dropped = 0;
    r.complete = false;
    queue_.push(Event::inject(specs_[i].start_ns, FlowId{static_cast<std::uint32_t>(i)}, 0));
  }

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
        on_arrive(e);
        break;
    }
  }
  // ------------------------------------------------------------------------

  stats_.virtual_end_ns = now_;
  for (const FlowResult& r : results_) {
    if (r.complete) {
      ++stats_.flows_complete;
    } else {
      ++stats_.flows_incomplete;
    }
  }
  return stats_;
}

void Simulation::on_inject(const Event& e) {
  const FlowId f = e.flow();
  const FlowSpec& spec = specs_[f.index()];
  const std::uint32_t idx = e.packet_index();

  const Bytes mtu = spec.mtu_bytes;
  const Bytes offset = static_cast<Bytes>(idx) * mtu;
  const Bytes remaining = spec.size_bytes - offset;
  const auto bytes = static_cast<std::uint16_t>(remaining < mtu ? remaining : mtu);

  const std::span<const PortId> nics = fabric_.ports_of(spec.src);
  assert(nics.size() == 1 && "a host has exactly one NIC");
  const PortId nic = nics[0];

  stats_.bytes_injected += bytes;
  enqueue(nic, Packet{f, idx, bytes});

  // Source pacing lives entirely in this schedule: the next packet is released
  // one serialisation time later, which is exactly NIC line rate. The host
  // queue therefore never backs up in M0. M2 replaces this with ack clocking.
  if (offset + bytes < spec.size_bytes) {
    const Nanos period = serialization_ns(bytes, fabric_.port(nic).rate);
    queue_.push(Event::inject(now_ + period, f, idx + 1));
  }
}

void Simulation::on_tx_complete(const Event& e) {
  const PortId pid = e.port();
  Port& p = fabric_.port(pid);
  const Packet pkt = p.in_flight;

  ++p.packets_sent;
  p.bytes_sent += pkt.bytes;

  queue_.push(Event::arrive(now_ + p.prop_delay_ns, pid, pkt.flow, pkt.index, pkt.bytes));

  if (!p.queue.empty()) {
    start_tx(pid);
  } else {
    p.busy = false;
  }
}

void Simulation::on_arrive(const Event& e) {
  const NodeId at = fabric_.port(e.port()).peer_node;
  const FlowId f = e.flow();
  const FlowSpec& spec = specs_[f.index()];

  if (at == spec.dst) {
    FlowResult& r = results_[f.index()];
    ++r.packets_delivered;
    r.delivered_bytes += e.bytes;
    stats_.bytes_delivered += e.bytes;
    if (r.packets_delivered == r.packets_total) {
      r.finish_ns = now_;
      r.complete = true;
    }
    return;
  }

  assert(!fabric_.node(at).is_host && "a host is never a transit node");
  enqueue(select_output(at, spec), Packet{f, e.packet_index(), e.bytes});
}

void Simulation::enqueue(PortId pid, const Packet& pkt) {
  Port& p = fabric_.port(pid);

  if (!p.queue.try_push(pkt)) {
    // Drop-tail. No retransmission in M0, so the owning flow will simply never
    // complete and run() reports it as incomplete.
    ++p.packets_dropped;
    p.bytes_dropped += pkt.bytes;
    stats_.bytes_dropped += pkt.bytes;
    ++results_[pkt.flow.index()].packets_dropped;
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

PortId Simulation::select_output(NodeId at, const FlowSpec& spec) const noexcept {
  const std::uint32_t dst_host = fabric_.host_index(spec.dst);
  const std::span<const PortId> cand = routes_.candidates(at, dst_host);
  assert(!cand.empty() && "no route to destination");

  // M0 policy: always the first shortest-path candidate. Deliberately trivial
  // and deterministic; M1's ECMP hashes the flow key across this same span and
  // M3's adaptive policy picks by queue occupancy.
  return cand[0];
}

std::uint64_t Simulation::result_digest() const noexcept {
  std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
  for (const FlowResult& r : results_) {
    const std::uint64_t packed = (static_cast<std::uint64_t>(r.packets_delivered) << 32) |
                                 static_cast<std::uint64_t>(r.packets_dropped);
    h = splitmix64(h ^ r.start_ns);
    h = splitmix64(h ^ r.finish_ns);
    h = splitmix64(h ^ r.delivered_bytes);
    h = splitmix64(h ^ packed);
    h = splitmix64(h ^ static_cast<std::uint64_t>(r.complete));
  }
  return h;
}

}  // namespace fabric
