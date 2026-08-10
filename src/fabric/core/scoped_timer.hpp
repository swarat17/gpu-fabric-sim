#pragma once

#include <chrono>

namespace fabric {

// ---------------------------------------------------------------------------
// RAII wall-clock timer.
//
// This is the one place a double appears near the core, and it is deliberate:
// it measures how long the *simulator* took to run, which is host-machine
// instrumentation, not simulation state. The no-floating-point rule in
// units.hpp constrains modelled quantities -- virtual time, bytes, queue
// depths -- because those must be bit-reproducible. Wall-clock seconds are
// expected to differ run to run and are never fed back into the model.
// ---------------------------------------------------------------------------
class ScopedTimer {
 public:
  explicit ScopedTimer(double& out_seconds) noexcept
      : out_(&out_seconds), start_(Clock::now()) {}

  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
  ScopedTimer(ScopedTimer&&) = delete;
  ScopedTimer& operator=(ScopedTimer&&) = delete;

  ~ScopedTimer() {
    *out_ = std::chrono::duration<double>(Clock::now() - start_).count();
  }

 private:
  using Clock = std::chrono::steady_clock;

  double* out_;
  Clock::time_point start_;
};

}  // namespace fabric
