#include "fabric/metrics/fct.hpp"

#include <algorithm>
#include <cassert>

namespace fabric {

Nanos percentile_ns(std::span<const Nanos> sorted, std::uint32_t permille) noexcept {
  assert(!sorted.empty());
  assert(permille <= 1000);
  assert(std::is_sorted(sorted.begin(), sorted.end()));

  const auto n = static_cast<std::uint64_t>(sorted.size());
  // rank = ceil(permille * n / 1000), and never below 1.
  std::uint64_t rank = (static_cast<std::uint64_t>(permille) * n + 999) / 1000;
  if (rank == 0) {
    rank = 1;
  }
  return sorted[static_cast<std::size_t>(rank - 1)];
}

std::vector<Nanos> completed_fcts(std::span<const FlowResult> results) {
  std::vector<Nanos> v;
  v.reserve(results.size());
  for (const FlowResult& r : results) {
    if (r.complete) {
      v.push_back(r.fct_ns());
    }
  }
  std::sort(v.begin(), v.end());
  return v;
}

FctSummary summarize_fct(std::span<const FlowResult> results) {
  FctSummary s;
  s.flows_total = static_cast<std::uint32_t>(results.size());

  const std::vector<Nanos> fcts = completed_fcts(results);
  s.flows_complete = static_cast<std::uint32_t>(fcts.size());
  s.flows_incomplete = s.flows_total - s.flows_complete;
  if (fcts.empty()) {
    return s;
  }

  s.min_ns = fcts.front();
  s.max_ns = fcts.back();
  s.p50_ns = percentile_ns(fcts, 500);
  s.p90_ns = percentile_ns(fcts, 900);
  s.p99_ns = percentile_ns(fcts, 990);

  Nanos sum = 0;
  for (const Nanos v : fcts) {
    sum += v;
  }
  s.mean_ns = sum / static_cast<Nanos>(fcts.size());
  return s;
}

}  // namespace fabric
