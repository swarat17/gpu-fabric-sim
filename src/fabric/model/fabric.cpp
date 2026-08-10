#include "fabric/model/fabric.hpp"

#include <cassert>

namespace fabric {

void Fabric::reset_state() noexcept {
  for (Port& p : ports_) {
    p.busy = false;
    p.in_flight = Packet{};
    p.queue.init(p.queue.capacity());
    p.packets_sent = 0;
    p.bytes_sent = 0;
    p.packets_dropped = 0;
    p.bytes_dropped = 0;
  }
}

NodeId FabricBuilder::add_switch() {
  const auto id = static_cast<std::uint32_t>(is_host_.size());
  is_host_.push_back(0);
  return NodeId{id};
}

NodeId FabricBuilder::add_host() {
  const auto id = static_cast<std::uint32_t>(is_host_.size());
  is_host_.push_back(1);
  return NodeId{id};
}

void FabricBuilder::connect(NodeId a, NodeId b, const LinkParams& params) {
  assert(a.index() < is_host_.size() && b.index() < is_host_.size());
  assert(a != b && "self-links are not modelled");
  edges_.push_back(Edge{a, b, params});
}

Fabric FabricBuilder::build() const {
  Fabric f;
  const std::size_t n = is_host_.size();

  f.nodes_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    f.nodes_[i].is_host = is_host_[i] != 0;
  }

  // Pass 1: count ports per node so each node's run can be laid out contiguously.
  std::vector<std::uint32_t> degree(n, 0);
  for (const Edge& e : edges_) {
    ++degree[e.a.index()];
    ++degree[e.b.index()];
  }

  std::uint32_t running = 0;
  for (std::size_t i = 0; i < n; ++i) {
    f.nodes_[i].first_port = running;
    f.nodes_[i].port_count = 0;  // filled as ports are appended in pass 2
    running += degree[i];
  }

  const std::size_t total_ports = static_cast<std::size_t>(running);
  f.ports_.resize(total_ports);
  f.port_ids_.resize(total_ports);

  // Pass 2: materialise two directed ports per edge, each into its owner's run.
  auto claim_slot = [&](NodeId owner) -> std::uint32_t {
    Node& nd = f.nodes_[owner.index()];
    const std::uint32_t slot = nd.first_port + nd.port_count;
    ++nd.port_count;
    return slot;
  };

  for (const Edge& e : edges_) {
    const std::uint32_t sa = claim_slot(e.a);
    const std::uint32_t sb = claim_slot(e.b);

    Port& pa = f.ports_[sa];
    Port& pb = f.ports_[sb];

    pa.owner = e.a;
    pa.peer_node = e.b;
    pa.peer_port = PortId{sb};
    pa.rate = e.params.rate;
    pa.prop_delay_ns = e.params.prop_delay_ns;
    pa.queue.init(e.params.queue_capacity_pkts);

    pb.owner = e.b;
    pb.peer_node = e.a;
    pb.peer_port = PortId{sa};
    pb.rate = e.params.rate;
    pb.prop_delay_ns = e.params.prop_delay_ns;
    pb.queue.init(e.params.queue_capacity_pkts);

    f.port_ids_[sa] = PortId{sa};
    f.port_ids_[sb] = PortId{sb};
  }

  // Dense host indexing for routing-table keys.
  f.host_index_.assign(n, Id<NodeTag>::kInvalidValue);
  for (std::size_t i = 0; i < n; ++i) {
    if (f.nodes_[i].is_host) {
      f.host_index_[i] = static_cast<std::uint32_t>(f.hosts_.size());
      f.hosts_.push_back(NodeId{static_cast<std::uint32_t>(i)});
    }
  }

  return f;
}

}  // namespace fabric
