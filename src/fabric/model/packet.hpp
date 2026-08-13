#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fabric/core/ids.hpp"

namespace fabric {

// Data and acknowledgement share the queues and the wire, because they do in a
// real fabric: an ack is a small packet that competes for the same buffers and
// gets dropped by the same drop-tail rule. Modelling acks as free would make the
// transport's feedback loop faster than any real one.
enum class PacketKind : std::uint8_t {
  Data = 0,
  Ack = 1,
};

// What actually sits in an output queue. The payload is never materialised --
// only its length matters to the model -- so a packet is 12 bytes of metadata.
struct Packet {
  FlowId flow{};
  std::uint32_t index = 0;  // packet index within the flow
  std::uint16_t bytes = 0;
  PacketKind kind = PacketKind::Data;
  // Which transmission attempt this is. Carried so that a retransmission
  // timer belonging to a superseded attempt can be recognised and ignored;
  // see Simulation::on_timeout.
  std::uint8_t attempt = 0;
};

static_assert(sizeof(Packet) <= 12);

// ---------------------------------------------------------------------------
// Fixed-capacity drop-tail queue.
//
// Capacity is fixed at topology-construction time and never grows: a real
// switch port has a finite buffer, and equally, allocating on the hot path
// would put malloc into the middle of the event loop. Overflow drops the
// arriving packet, which is drop-tail by definition.
// ---------------------------------------------------------------------------
class PacketRing {
 public:
  void init(std::size_t capacity) {
    buf_.assign(capacity, Packet{});
    capacity_ = capacity;
    head_ = 0;
    count_ = 0;
  }

  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] bool full() const noexcept { return count_ == capacity_; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // Returns false if the queue was full, i.e. the packet is dropped.
  [[nodiscard]] bool try_push(const Packet& p) noexcept {
    if (count_ == capacity_) {
      return false;
    }
    std::size_t tail = head_ + count_;
    if (tail >= capacity_) {
      tail -= capacity_;
    }
    buf_[tail] = p;
    ++count_;
    return true;
  }

  Packet pop() noexcept {
    Packet p = buf_[head_];
    ++head_;
    if (head_ == capacity_) {
      head_ = 0;
    }
    --count_;
    return p;
  }

 private:
  std::vector<Packet> buf_;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

}  // namespace fabric
