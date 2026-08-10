#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fabric/core/ids.hpp"
#include "fabric/model/fabric.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Shortest-path next-hop table in CSR layout.
//
// candidates(at, dst) returns *every* output port that lies on a minimum-hop
// path to dst, so the table is already the ECMP candidate set. M0 takes
// element 0 and calls it static routing; M1's ECMP hashes the flow key across
// the span; M3's adaptive policy picks by queue occupancy. Same table, three
// policies -- which is why this is built with multipath from the start rather
// than being retrofitted later.
//
// Layout: one contiguous run of PortIds per (node, destination host) cell,
// indexed through offsets_. Flat and read-only after construction.
// ---------------------------------------------------------------------------
class RouteTable {
 public:
  // Unit-weight BFS from each destination host. Hop count is the right notion
  // of distance for a Clos: every minimum-hop path is also a legitimate
  // capacity-equivalent path.
  [[nodiscard]] static RouteTable build_shortest_path(const Fabric& f);

  [[nodiscard]] std::span<const PortId> candidates(NodeId at,
                                                   std::uint32_t dst_host) const noexcept {
    const std::size_t cell = at.index() * num_hosts_ + dst_host;
    const std::uint32_t begin = offsets_[cell];
    const std::uint32_t end = offsets_[cell + 1];
    return {entries_.data() + begin, static_cast<std::size_t>(end - begin)};
  }

  [[nodiscard]] std::size_t num_hosts() const noexcept { return num_hosts_; }

 private:
  std::vector<PortId> entries_;
  std::vector<std::uint32_t> offsets_;  // node_count * num_hosts + 1
  std::size_t num_hosts_ = 0;
};

}  // namespace fabric
