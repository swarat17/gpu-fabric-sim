#include "fabric/core/simulation.hpp"

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

  const auto id = static_cast<std::uint32_t>(specs_.size());
  specs_.push_back(spec);

  // Host dense indices stand in for IP addresses. splitmix64 mixes small
  // sequential integers as well as it mixes anything else -- that is what it was
  // designed for -- so nothing is gained by dressing them up as dotted quads.
  FlowRoute fr;
  fr.dst_host = fabric_.host_index(spec.dst);
  fr.key = FlowKey{fabric_.host_index(spec.src), fr.dst_host, spec.src_port, spec.dst_port,
                   spec.protocol};
  froutes_.push_back(fr);

  FlowResult r;
  r.start_ns = spec.start_ns;
  const Bytes mtu = spec.mtu_bytes;
  r.packets_total = static_cast<std::uint32_t>((spec.size_bytes + mtu - 1) / mtu);
  results_.push_back(r);

  return FlowId{id};
}

void Simulation::begin_run() {
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
}

void Simulation::end_run() {
  stats_.virtual_end_ns = now_;
  for (const FlowResult& r : results_) {
    if (r.complete) {
      ++stats_.flows_complete;
    } else {
      ++stats_.flows_incomplete;
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
    }
  }
  // ------------------------------------------------------------------------

  end_run();
  return stats_;
}

RunStats Simulation::run() { return run(StaticFirstPolicy{}); }

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

  // Source pacing lives entirely in this schedule. At load_permille == 1000 the
  // period is exactly one serialisation time, i.e. NIC line rate, and the host
  // queue never backs up. Below that the source simply idles between packets.
  // M2 replaces the whole scheme with ack clocking.
  if (offset + bytes < spec.size_bytes) {
    const Nanos ser = serialization_ns(bytes, fabric_.port(nic).rate);
    const Nanos period = (ser * 1000ULL) / spec.load_permille;
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

template <RoutingPolicy Policy>
void Simulation::on_arrive(const Event& e, const Policy& policy) {
  const NodeId at = fabric_.port(e.port()).peer_node;
  const FlowId f = e.flow();

  // Routing only ever forwards toward the destination and a host is never a
  // transit node, so arriving at a host means arriving home. Cheaper than
  // reloading the FlowSpec to compare node ids; the assert keeps it honest.
  if (fabric_.node(at).is_host) {
    assert(at == specs_[f.index()].dst);
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

  const FlowRoute& fr = froutes_[f.index()];
  const std::span<const PortId> cand = routes_.candidates(at, fr.dst_host);
  assert(!cand.empty() && "no route to destination");

  enqueue(policy.select(at, fr.key, cand, now_), Packet{f, e.packet_index(), e.bytes});
}

void Simulation::enqueue(PortId pid, const Packet& pkt) {
  Port& p = fabric_.port(pid);

  if (!p.queue.try_push(pkt)) {
    // Drop-tail. No retransmission yet, so the owning flow will simply never
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

std::uint64_t Simulation::result_digest() const noexcept {
  std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
  for (const FlowResult& r : results_) {
    const std::uint64_t packed = (static_cast<std::uint64_t>(r.packets_delivered) << 32) |
                                 static_cast<std::uint64_t>(r.packets_dropped);
    h = mix64(h ^ r.start_ns);
    h = mix64(h ^ r.finish_ns);
    h = mix64(h ^ r.delivered_bytes);
    h = mix64(h ^ packed);
    h = mix64(h ^ static_cast<std::uint64_t>(r.complete));
  }
  return h;
}

// Every policy the binary ships, instantiated at the end of the translation
// unit where all the templates it calls are defined. This is what keeps the hot
// loop out of every header that mentions a Simulation; M3 adds one more line.
template RunStats Simulation::run<StaticFirstPolicy>(const StaticFirstPolicy&);
template RunStats Simulation::run<EcmpPolicy>(const EcmpPolicy&);

}  // namespace fabric
