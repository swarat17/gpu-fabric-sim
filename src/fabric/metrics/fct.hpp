#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fabric/core/simulation.hpp"
#include "fabric/core/units.hpp"

namespace fabric {

// ---------------------------------------------------------------------------
// Flow completion time statistics. Cold path: this runs once at the end of a
// run and may allocate, sort and use anything it likes.
//
// Still integer-only, and for a reason beyond consistency: a percentile is the
// value of an actual observation, so there is nothing here to interpolate and
// no excuse for introducing floating point into a reported number.
// ---------------------------------------------------------------------------

// Nearest-rank percentile, the definition stated in the README and in
// docs/validation.md: the observation at rank ceil(permille/1000 * n) of the
// sorted sample. No interpolation between neighbours, so every reported
// percentile is a flow that really finished at that time. `sorted` must be
// ascending and non-empty.
[[nodiscard]] Nanos percentile_ns(std::span<const Nanos> sorted, std::uint32_t permille) noexcept;

struct FctSummary {
  std::uint32_t flows_total = 0;
  std::uint32_t flows_complete = 0;
  std::uint32_t flows_incomplete = 0;

  Nanos min_ns = 0;
  Nanos p50_ns = 0;
  Nanos p90_ns = 0;
  Nanos p99_ns = 0;
  Nanos max_ns = 0;
  Nanos mean_ns = 0;

  // False when some flow never finished. Percentiles are then computed over the
  // survivors only, which silently *improves* every one of them -- the flows
  // that were doing worst are exactly the ones missing. Callers must say so
  // loudly rather than quietly reporting the number; see docs/validation.md.
  [[nodiscard]] bool complete() const noexcept { return flows_incomplete == 0; }
};

// Ascending FCTs of the flows that finished. The raw vector is what M4's CSV
// and bootstrap resampling consume.
[[nodiscard]] std::vector<Nanos> completed_fcts(std::span<const FlowResult> results);

[[nodiscard]] FctSummary summarize_fct(std::span<const FlowResult> results);

}  // namespace fabric
