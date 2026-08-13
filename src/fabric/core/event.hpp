#pragma once

#include <cstdint>
#include <type_traits>

#include "fabric/core/ids.hpp"
#include "fabric/core/units.hpp"
#include "fabric/model/packet.hpp"

namespace fabric {

enum class EventType : std::uint8_t {
  // A flow may send. Fired once when the flow starts (or is released by the
  // flow it depends on) and again whenever a paced send has to wait. What the
  // sender is actually allowed to put on the wire is decided by the window, so
  // this event is an opportunity, not a command.
  Inject = 0,
  // An output port finished clocking its in-flight packet onto the wire.
  TxComplete = 1,
  // A packet reached the far end of the link it was traversing.
  Arrive = 2,
  // A retransmission timer expired. Carries the attempt number it belongs to,
  // so timers superseded by a later attempt can be recognised and dropped
  // rather than cancelled -- there is no cheap way to remove an entry from the
  // middle of a binary heap, and lazy invalidation is the standard answer.
  Timeout = 3,
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
  std::uint32_t a = 0;      // PortId for Arrive/TxComplete; attempt for Timeout
  std::uint32_t b = 0;      // FlowId
  std::uint32_t c = 0;      // packet index within the flow
  std::uint16_t bytes = 0;  // packet size; bounded by kMaxPacketBytes
  EventType type = EventType::Inject;
  PacketKind kind = PacketKind::Data;

  [[nodiscard]] constexpr PortId port() const noexcept { return PortId{a}; }
  [[nodiscard]] constexpr FlowId flow() const noexcept { return FlowId{b}; }
  [[nodiscard]] constexpr std::uint32_t packet_index() const noexcept { return c; }
  [[nodiscard]] constexpr std::uint8_t attempt() const noexcept {
    return static_cast<std::uint8_t>(a);
  }

  [[nodiscard]] static constexpr Event inject(Nanos t, FlowId f) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::Inject;
    e.b = f.value();
    return e;
  }

  [[nodiscard]] static constexpr Event tx_complete(Nanos t, PortId p) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::TxComplete;
    e.a = p.value();
    return e;
  }

  [[nodiscard]] static constexpr Event arrive(Nanos t, PortId traversed, const Packet& pkt) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::Arrive;
    e.kind = pkt.kind;
    e.a = traversed.value();
    e.b = pkt.flow.value();
    e.c = pkt.index;
    e.bytes = pkt.bytes;
    return e;
  }

  [[nodiscard]] static constexpr Event timeout(Nanos t, FlowId f, std::uint32_t pkt,
                                               std::uint8_t attempt) noexcept {
    Event e;
    e.time_ns = t;
    e.type = EventType::Timeout;
    e.a = attempt;
    e.b = f.value();
    e.c = pkt;
    return e;
  }
};

static_assert(sizeof(Event) == 32, "Event must stay at 32 bytes; it is the hot-path payload");
static_assert(alignof(Event) == 8);
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace fabric
