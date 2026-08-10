#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/packet.hpp"

namespace fabric {

struct LinkParams {
  BitsPerSec rate = gbps(100);
  Nanos prop_delay_ns = 100;
  std::size_t queue_capacity_pkts = 128;
};

struct Node {
  std::uint32_t first_port = 0;  // ports of a node are contiguous
  std::uint32_t port_count = 0;
  bool is_host = false;
};

// One directed link. See ids.hpp for why there is no separate Link entity.
struct Port {
  NodeId owner{};
  NodeId peer_node{};
  PortId peer_port{};

  BitsPerSec rate = 0;
  Nanos prop_delay_ns = 0;

  bool busy = false;
  Packet in_flight{};
  PacketRing queue;

  // Telemetry counters. M3 layers aged sampling on top of these; they are
  // plain adds on the hot path.
  std::uint64_t packets_sent = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t packets_dropped = 0;
  std::uint64_t bytes_dropped = 0;
};

// ---------------------------------------------------------------------------
// The fabric: nodes and directed links, plus the dense host indexing the
// routing tables are built against. Immutable in shape once built; only the
// per-port queue state and counters mutate during a run.
// ---------------------------------------------------------------------------
class Fabric {
 public:
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t port_count() const noexcept { return ports_.size(); }
  [[nodiscard]] std::size_t host_count() const noexcept { return hosts_.size(); }

  [[nodiscard]] const Node& node(NodeId n) const noexcept { return nodes_[n.index()]; }
  [[nodiscard]] Port& port(PortId p) noexcept { return ports_[p.index()]; }
  [[nodiscard]] const Port& port(PortId p) const noexcept { return ports_[p.index()]; }

  [[nodiscard]] std::span<const PortId> ports_of(NodeId n) const noexcept {
    const Node& nd = nodes_[n.index()];
    return {port_ids_.data() + nd.first_port, nd.port_count};
  }

  [[nodiscard]] std::span<const NodeId> hosts() const noexcept { return hosts_; }

  // Dense 0..host_count-1 index for a host node; used to key routing tables.
  [[nodiscard]] std::uint32_t host_index(NodeId n) const noexcept {
    return host_index_[n.index()];
  }
  [[nodiscard]] NodeId host_at(std::uint32_t host_index) const noexcept {
    return hosts_[host_index];
  }

  void reset_state() noexcept;

 private:
  friend class FabricBuilder;

  std::vector<Node> nodes_;
  std::vector<Port> ports_;
  std::vector<PortId> port_ids_;  // flat, grouped by owning node
  std::vector<NodeId> hosts_;
  std::vector<std::uint32_t> host_index_;
};

// Two-phase construction: edges are collected first, then materialised so that
// every node's ports land in one contiguous run. Doing it in one pass would
// interleave ports of different nodes and cost a cache miss per hop forever
// after. This is cold-path code; the layout it produces is not.
class FabricBuilder {
 public:
  NodeId add_switch();
  NodeId add_host();
  void connect(NodeId a, NodeId b, const LinkParams& params);

  [[nodiscard]] Fabric build() const;

 private:
  struct Edge {
    NodeId a;
    NodeId b;
    LinkParams params;
  };

  std::vector<std::uint8_t> is_host_;
  std::vector<Edge> edges_;
};

}  // namespace fabric
