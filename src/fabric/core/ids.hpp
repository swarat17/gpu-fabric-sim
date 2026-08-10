#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace fabric {

// ---------------------------------------------------------------------------
// Handles, not pointers.
//
// Every entity in the fabric is addressed by a dense uint32 index into a
// contiguous vector owned by Simulation. That buys three things at once:
//   * stable references across vector reallocation,
//   * cache-friendly layout on the hot path,
//   * determinism -- nothing is ever ordered by a pointer value.
//
// The distinct tag types exist because mixing a PortId with a NodeId is the
// single likeliest silent bug in this codebase; both are just uint32 indices
// and the compiler would otherwise be happy to swap them.
// ---------------------------------------------------------------------------
template <class Tag>
class Id {
 public:
  using value_type = std::uint32_t;
  static constexpr value_type kInvalidValue = 0xFFFF'FFFFu;

  constexpr Id() noexcept = default;
  constexpr explicit Id(value_type v) noexcept : v_(v) {}

  [[nodiscard]] constexpr value_type value() const noexcept { return v_; }
  [[nodiscard]] constexpr std::size_t index() const noexcept {
    return static_cast<std::size_t>(v_);
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return v_ != kInvalidValue; }

  [[nodiscard]] constexpr bool operator==(const Id&) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;

  [[nodiscard]] static constexpr Id invalid() noexcept { return Id{kInvalidValue}; }

 private:
  value_type v_ = kInvalidValue;
};

struct NodeTag {};
struct PortTag {};
struct FlowTag {};

// A node is a switch or a host; hosts are the ones with exactly one port.
using NodeId = Id<NodeTag>;
// A PortId names a *directed* link: the output side of one endpoint. There is
// deliberately no separate Link entity -- congestion is directional, so the
// queue, the counters and the telemetry all belong to the direction, and
// collapsing the two removes an indirection from the hot path.
using PortId = Id<PortTag>;
using FlowId = Id<FlowTag>;

}  // namespace fabric
