#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <random>

#include "fabric/core/event_queue.hpp"

using namespace fabric;

TEST_CASE("event queue pops in non-decreasing time order") {
  EventQueue q;
  std::mt19937_64 rng(12345);

  constexpr int kCount = 2000;
  for (int i = 0; i < kCount; ++i) {
    q.push(Event::inject(rng() % 10000, FlowId{0}));
  }

  Nanos last = 0;
  std::size_t popped = 0;
  while (!q.empty()) {
    const Event e = q.pop();
    CHECK(e.time_ns >= last);
    last = e.time_ns;
    ++popped;
  }
  CHECK(popped == static_cast<std::size_t>(kCount));
}

TEST_CASE("events at equal virtual time resolve in insertion order") {
  // The determinism guarantee. A binary heap imposes no order among equal
  // keys, so two events scheduled for the same nanosecond could otherwise come
  // back in either order depending on heap shape -- and every published number
  // would stop being reproducible. EventQueue::push stamps a sequence number
  // to make the order total.
  EventQueue q;
  constexpr std::uint32_t kCount = 64;
  for (std::uint32_t i = 0; i < kCount; ++i) {
    q.push(Event::timeout(1000, FlowId{0}, i, 0));
  }
  for (std::uint32_t i = 0; i < kCount; ++i) {
    const Event e = q.pop();
    CHECK(e.time_ns == 1000);
    CHECK(e.packet_index() == i);
  }
  CHECK(q.empty());
}

TEST_CASE("interleaved push and pop preserves the heap invariant") {
  EventQueue q;
  std::mt19937_64 rng(99);

  Nanos last = 0;
  std::size_t popped = 0;
  for (int round = 0; round < 5000; ++round) {
    if (q.empty() || (rng() % 3) != 0) {
      q.push(Event::inject(last + (rng() % 500), FlowId{0}));
    } else {
      const Event e = q.pop();
      CHECK(e.time_ns >= last);
      last = e.time_ns;
      ++popped;
    }
  }
  CHECK(popped > 0);
}

TEST_CASE("typed accessors round-trip through the generic event slots") {
  EventQueue q;
  q.push(Event::arrive(42, PortId{7}, Packet{FlowId{3}, 11, 1500, PacketKind::Data, 0}));
  const Event e = q.pop();
  CHECK(e.type == EventType::Arrive);
  CHECK(e.kind == PacketKind::Data);
  CHECK(e.time_ns == 42);
  CHECK(e.port() == PortId{7});
  CHECK(e.flow() == FlowId{3});
  CHECK(e.packet_index() == 11);
  CHECK(e.bytes == 1500);
}

TEST_CASE("an ack event carries its kind, and a timer its attempt number") {
  // Two fields the transport depends on: routing an arrival needs to know
  // whether it is data or an ack, and a retransmission timer has to identify
  // which attempt it belongs to so a superseded one can be ignored.
  EventQueue q;
  q.push(Event::arrive(7, PortId{1}, Packet{FlowId{2}, 5, 64, PacketKind::Ack, 0}));
  q.push(Event::timeout(9, FlowId{2}, 5, 3));

  const Event ack = q.pop();
  CHECK(ack.type == EventType::Arrive);
  CHECK(ack.kind == PacketKind::Ack);
  CHECK(ack.bytes == 64);

  const Event timer = q.pop();
  CHECK(timer.type == EventType::Timeout);
  CHECK(timer.flow() == FlowId{2});
  CHECK(timer.packet_index() == 5);
  CHECK(timer.attempt() == 3);
}
