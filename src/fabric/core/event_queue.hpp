#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "fabric/core/event.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Binary min-heap over a flat vector of POD events.
//
// Hand-written rather than std::priority_queue for two reasons: the sift
// routines show up by name in a profile, and milestone M5 A/Bs this against a
// bucketed timing wheel (link delays give the simulation a bounded lookahead,
// which is exactly the precondition a wheel needs). Keeping the interface
// narrow -- push/pop/empty -- is what makes that swap a contained change.
// ---------------------------------------------------------------------------
class EventQueue {
 public:
  void reserve(std::size_t n) { heap_.reserve(n); }

  [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }

  // Assigning seq here, rather than at the call site, is the determinism
  // guarantee: events at equal virtual time always resolve in the order they
  // were scheduled, and that order is itself a deterministic function of the
  // dispatch sequence.
  void push(Event e) {
    e.seq = next_seq_++;
    heap_.push_back(e);
    sift_up(heap_.size() - 1);
  }

  [[nodiscard]] const Event& top() const noexcept { return heap_.front(); }

  Event pop() noexcept {
    Event out = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) {
      sift_down(0);
    }
    return out;
  }

  void clear() noexcept {
    heap_.clear();
    next_seq_ = 0;
  }

  // Number of events ever pushed. Reported as the simulator's own throughput
  // denominator -- see README "Simulator performance".
  [[nodiscard]] std::uint64_t total_pushed() const noexcept { return next_seq_; }

 private:
  [[nodiscard]] static bool earlier(const Event& x, const Event& y) noexcept {
    if (x.time_ns != y.time_ns) {
      return x.time_ns < y.time_ns;
    }
    return x.seq < y.seq;
  }

  void sift_up(std::size_t i) noexcept {
    Event held = heap_[i];
    while (i > 0) {
      const std::size_t parent = (i - 1) / 2;
      if (!earlier(held, heap_[parent])) {
        break;
      }
      heap_[i] = heap_[parent];
      i = parent;
    }
    heap_[i] = held;
  }

  void sift_down(std::size_t i) noexcept {
    const std::size_t n = heap_.size();
    Event held = heap_[i];
    for (;;) {
      const std::size_t left = 2 * i + 1;
      if (left >= n) {
        break;
      }
      const std::size_t right = left + 1;
      const std::size_t child = (right < n && earlier(heap_[right], heap_[left])) ? right : left;
      if (!earlier(heap_[child], held)) {
        break;
      }
      heap_[i] = heap_[child];
      i = child;
    }
    heap_[i] = held;
  }

  std::vector<Event> heap_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace fabric
