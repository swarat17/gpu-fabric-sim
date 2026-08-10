#include "fabric/routing/route_table.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace fabric {

namespace {

constexpr std::uint32_t kUnreachable = std::numeric_limits<std::uint32_t>::max();

// Hop distance from `source` to every node, over the undirected graph.
void bfs_distances(const Fabric& f, NodeId source, std::vector<std::uint32_t>& dist,
                   std::vector<NodeId>& frontier, std::vector<NodeId>& next) {
  dist.assign(f.node_count(), kUnreachable);
  dist[source.index()] = 0;

  frontier.clear();
  frontier.push_back(source);

  std::uint32_t depth = 0;
  while (!frontier.empty()) {
    ++depth;
    next.clear();
    for (const NodeId u : frontier) {
      for (const PortId p : f.ports_of(u)) {
        const NodeId v = f.port(p).peer_node;
        if (dist[v.index()] == kUnreachable) {
          dist[v.index()] = depth;
          next.push_back(v);
        }
      }
    }
    frontier.swap(next);
  }
}

}  // namespace

RouteTable RouteTable::build_shortest_path(const Fabric& f) {
  RouteTable rt;
  rt.num_hosts_ = f.host_count();

  const std::size_t cells = f.node_count() * rt.num_hosts_;
  rt.offsets_.assign(cells + 1, 0);

  std::vector<std::uint32_t> dist;
  std::vector<NodeId> frontier;
  std::vector<NodeId> next;

  // Per-destination next-hop sets, collected before the CSR is packed so the
  // offsets can be computed in one prefix sum.
  std::vector<std::vector<PortId>> per_cell(cells);

  for (std::uint32_t h = 0; h < rt.num_hosts_; ++h) {
    const NodeId dst = f.host_at(h);
    bfs_distances(f, dst, dist, frontier, next);

    for (std::size_t u = 0; u < f.node_count(); ++u) {
      const std::uint32_t du = dist[u];
      if (du == 0 || du == kUnreachable) {
        continue;  // the destination itself, or a disconnected node
      }
      std::vector<PortId>& out = per_cell[u * rt.num_hosts_ + h];
      for (const PortId p : f.ports_of(NodeId{static_cast<std::uint32_t>(u)})) {
        const NodeId v = f.port(p).peer_node;
        if (dist[v.index()] + 1 == du) {
          out.push_back(p);
        }
      }
    }
  }

  std::uint32_t running = 0;
  for (std::size_t c = 0; c < cells; ++c) {
    rt.offsets_[c] = running;
    running += static_cast<std::uint32_t>(per_cell[c].size());
  }
  rt.offsets_[cells] = running;

  rt.entries_.reserve(running);
  for (std::size_t c = 0; c < cells; ++c) {
    for (const PortId p : per_cell[c]) {
      rt.entries_.push_back(p);
    }
  }

  return rt;
}

}  // namespace fabric
