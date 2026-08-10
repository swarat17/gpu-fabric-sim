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
    q.push(Event::inject(rng() % 10000, FlowId{0}, static_cast<std::uint32_t>(i)));
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
    q.push(Event::inject(1000, FlowId{0}, i));
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
      q.push(Event::inject(last + (rng() % 500), FlowId{0}, 0));
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
  q.push(Event::arrive(42, PortId{7}, FlowId{3}, 11, 1500));
  const Event e = q.pop();
  CHECK(e.type == EventType::Arrive);
  CHECK(e.time_ns == 42);
  CHECK(e.port() == PortId{7});
  CHECK(e.flow() == FlowId{3});
  CHECK(e.packet_index() == 11);
  CHECK(e.bytes == 1500);
}
