#include <doctest.h>

#include <cstdint>
#include <vector>

#include "fabric/core/simulation.hpp"
#include "fabric/metrics/fct.hpp"

using namespace fabric;

// ---------------------------------------------------------------------------
// The percentile definition is part of the published claim, so it is pinned by
// tests rather than left to whichever convention a plotting library happens to
// use. Nearest rank: p99 of 100 samples is the 99th smallest, and every reported
// percentile is the completion time of a flow that actually existed.
// ---------------------------------------------------------------------------

namespace {

std::vector<Nanos> ascending(std::uint32_t n) {
  std::vector<Nanos> v;
  v.reserve(n);
  for (std::uint32_t i = 1; i <= n; ++i) {
    v.push_back(Nanos{i} * 10);
  }
  return v;
}

FlowResult done(Nanos start, Nanos finish) {
  FlowResult r;
  r.start_ns = start;
  r.finish_ns = finish;
  r.packets_total = 1;
  r.packets_delivered = 1;
  r.complete = true;
  return r;
}

}  // namespace

TEST_CASE("nearest-rank percentiles pick real observations") {
  const std::vector<Nanos> v = ascending(100);  // 10, 20, ... 1000

  CHECK(percentile_ns(v, 500) == 500);
  CHECK(percentile_ns(v, 900) == 900);
  CHECK(percentile_ns(v, 990) == 990);
  CHECK(percentile_ns(v, 1000) == 1000);
  // Below the first rank still returns the smallest sample, never zero.
  CHECK(percentile_ns(v, 0) == 10);
  CHECK(percentile_ns(v, 1) == 10);
}

TEST_CASE("percentiles round the rank up, so p99 of 10 samples is the largest") {
  const std::vector<Nanos> v = ascending(10);  // 10 .. 100
  // ceil(0.99 * 10) = 10.
  CHECK(percentile_ns(v, 990) == 100);
  // ceil(0.5 * 10) = 5.
  CHECK(percentile_ns(v, 500) == 50);
  // A single sample is every percentile of itself.
  const std::vector<Nanos> one{42};
  CHECK(percentile_ns(one, 0) == 42);
  CHECK(percentile_ns(one, 990) == 42);
}

TEST_CASE("summarize_fct reports over completed flows and counts the rest") {
  std::vector<FlowResult> results;
  results.push_back(done(0, 100));
  results.push_back(done(0, 300));
  results.push_back(done(50, 250));  // fct 200

  FlowResult stuck;
  stuck.packets_total = 10;
  stuck.packets_delivered = 3;
  stuck.packets_dropped = 1;
  results.push_back(stuck);

  const FctSummary s = summarize_fct(results);

  CHECK(s.flows_total == 4);
  CHECK(s.flows_complete == 3);
  CHECK(s.flows_incomplete == 1);
  CHECK_FALSE(s.complete());

  CHECK(s.min_ns == 100);
  CHECK(s.max_ns == 300);
  CHECK(s.p50_ns == 200);
  CHECK(s.p99_ns == 300);
  CHECK(s.mean_ns == 200);
}

TEST_CASE("an all-incomplete run reports no percentiles rather than zeros that look fine") {
  std::vector<FlowResult> results;
  FlowResult stuck;
  stuck.packets_total = 5;
  results.push_back(stuck);

  const FctSummary s = summarize_fct(results);
  CHECK(s.flows_complete == 0);
  CHECK(s.flows_incomplete == 1);
  CHECK_FALSE(s.complete());
  CHECK(s.p99_ns == 0);
}

TEST_CASE("completed_fcts returns the raw sorted sample M4 will bootstrap over") {
  std::vector<FlowResult> results;
  results.push_back(done(0, 300));
  results.push_back(done(0, 100));
  results.push_back(done(0, 200));

  const std::vector<Nanos> v = completed_fcts(results);
  REQUIRE(v.size() == 3);
  CHECK(v[0] == 100);
  CHECK(v[1] == 200);
  CHECK(v[2] == 300);
}
