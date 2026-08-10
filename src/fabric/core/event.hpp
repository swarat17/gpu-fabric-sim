#pragma once

#include <cstdint>
#include <type_traits>

#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"

namespace fabric {

enum class EventType : std::uint8_t {
  // A flow releases its next packet into its host NIC. Each Inject schedules
  // the following one, so the release schedule *is* the source pacing model.
  // M2 replaces the fixed period with ack clocking; nothing else changes.
  Inject = 0,
  // An output port finished clocking its in-flight packet onto the wire.
  TxComplete = 1,
  // A packet reached the far end of the link it was traversing.
  Arrive = 2,
};

// ---------------------------------------------------------------------------
// Event is a 32-byte POD copied through the heap millions of times per run;
// every byte here is cache traffic on the hottest path in the program. The
// fields are generic slots rather than a union of per-type structs because the
// heap only ever compares (time_ns, seq) and moves whole objects -- the typed
// accessors below keep call sites honest at zero cost.
// ---------------------------------------------------------------------------
struct Event {
  Nanos time_ns = 0;
  // Total-order tiebreak, assigned by EventQueue::push. Two events at the same
  // virtual time must still have a deterministic order or runs stop being
  // reproducible; insertion order supplies it.
  std::uint64_t seq = 0;
  std::uint32_t a = 0;      // PortId, per type
  std::uint32_t b = 0;      // FlowId, per type
  std::uint32_t c = 0;      // packet index within the flow
  std::uint16_t bytes = 0;  // packet size; bounded by kMaxPacketBytes
  EventType type = EventType::Inject;
  std::uint8_t reserved = 0;

  [[nodiscard]] constexpr PortId port() const noexcept { return PortId{a}; }
  [[nodiscard]] constexpr FlowId flow() const noexcept { return FlowId{b}; }
  [[nodiscard]] constexpr std::uint32_t packet_index() const noexcept { return c; }

  [[nodiscard]] static constexpr Event inject(Nanos t, FlowId f, std::uint32_t pkt) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::Inject;
    e.b = f.value();
    e.c = pkt;
    return e;
  }

  [[nodiscard]] static constexpr Event tx_complete(Nanos t, PortId p) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::TxComplete;
    e.a = p.value();
    return e;
  }

  [[nodiscard]] static constexpr Event arrive(Nanos t, PortId traversed, FlowId f,
                                              std::uint32_t pkt, std::uint16_t bytes) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::Arrive;
    e.a = traversed.value();
    e.b = f.value();
    e.c = pkt;
    e.bytes = bytes;
    return e;
  }
};

static_assert(sizeof(Event) == 32, "Event must stay at 32 bytes; it is the hot-path payload");
static_assert(alignof(Event) == 8);
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace fabric
