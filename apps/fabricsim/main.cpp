#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>

#include "fabric/core/scoped_timer.hpp"
#include "fabric/harness/scenario.hpp"
#include "fabric/metrics/fct.hpp"

namespace {

void print_usage() {
  std::cout << "fabricsim -- GPU fabric congestion simulator\n"
               "\n"
               "Usage:\n"
               "  fabricsim [--scenario=NAME] [options]\n"
               "\n"
               "Scenarios:\n"
               "  smoke        single flow across a 2-spine / 2-leaf / 8-host fabric (default)\n"
               "  permutation  every host sends one flow across a k-ary fat-tree\n"
               "\n"
               "Options (permutation):\n"
               "  --k=N          fat-tree arity, even (default 8 -> 128 hosts)\n"
               "  --routing=X    ecmp | static (default ecmp)\n"
               "  --seed=N       run seed; drives the workload and the ECMP hash (default 0)\n"
               "  --bytes=N      bytes per flow (default 1000000)\n"
               "  --mtu=N        packet size in bytes (default 1500)\n"
               "  --load=N       offered load per source in permille, 1..1000 (default 1000)\n";
}

// --key=value, returning the value if `arg` matches `key`.
[[nodiscard]] bool match_value(std::string_view arg, std::string_view key,
                               std::string_view& out) noexcept {
  if (!arg.starts_with(key) || arg.size() <= key.size() || arg[key.size()] != '=') {
    return false;
  }
  out = arg.substr(key.size() + 1);
  return true;
}

[[nodiscard]] bool parse_u64(std::string_view s, std::uint64_t& out) noexcept {
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const std::from_chars_result r = std::from_chars(first, last, out);
  return r.ec == std::errc{} && r.ptr == last;
}

int run_smoke() {
  fabric::SmokeConfig cfg;
  fabric::SmokeScenario sc = fabric::build_smoke(cfg);

  double wall_seconds = 0.0;
  fabric::RunStats stats;
  {
    const fabric::ScopedTimer timer(wall_seconds);
    stats = sc.sim.run();
  }

  const fabric::FlowResult& r = sc.sim.results()[sc.flow.index()];

  std::cout << std::format(
      "topology     : {} spine / {} leaf / {} hosts, {} directed links\n"
      "flow         : {} bytes, mtu {}, {} packets, {} hops\n"
      "\n"
      "fct simulated: {} ns\n"
      "fct analytic : {} ns{}\n"
      "delta        : {} ns\n"
      "\n"
      "bytes        : injected {}, delivered {}, dropped {}\n"
      "flows        : {} complete, {} incomplete\n"
      "events       : {} processed in {:.4f} s wall ({:.2f} M events/s)\n",
      cfg.topology.spine_count, cfg.topology.leaf_count, sc.sim.fabric().host_count(),
      sc.sim.fabric().port_count(), cfg.flow_bytes, cfg.mtu_bytes, sc.packets, sc.hops,
      r.fct_ns(), sc.analytical_ns, sc.analytical_exact ? "" : "  (approximate: see build_smoke)",
      static_cast<std::int64_t>(r.fct_ns()) - static_cast<std::int64_t>(sc.analytical_ns),
      stats.bytes_injected, stats.bytes_delivered, stats.bytes_dropped, stats.flows_complete,
      stats.flows_incomplete, stats.events_processed, wall_seconds,
      wall_seconds > 0.0 ? static_cast<double>(stats.events_processed) / wall_seconds / 1e6 : 0.0);

  return (r.complete && stats.bytes_dropped == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_permutation(const fabric::PermutationConfig& cfg) {
  fabric::PermutationScenario sc = fabric::build_permutation(cfg);

  double wall_seconds = 0.0;
  fabric::RunStats stats;
  {
    const fabric::ScopedTimer timer(wall_seconds);
    stats = fabric::run_with_routing(sc.sim, sc.routing, sc.seed);
  }

  const fabric::FctSummary fct = fabric::summarize_fct(sc.sim.results());

  std::cout << std::format(
      "topology     : fat-tree k={}, {} hosts, {} switches, {} directed links\n"
      "paths        : {} equal-cost cross-pod\n"
      "routing      : {} (seed {})\n"
      "workload     : permutation, {} flows, {} bytes each, mtu {}, load {}/1000\n"
      "\n"
      "fct p50      : {} ns\n"
      "fct p90      : {} ns\n"
      "fct p99      : {} ns\n"
      "fct max      : {} ns\n"
      "best case    : {} ns  (uncongested lower bound)\n"
      "\n"
      "bytes        : injected {}, delivered {}, dropped {}\n"
      "flows        : {} complete, {} incomplete\n"
      "events       : {} processed in {:.4f} s wall ({:.2f} M events/s)\n"
      "digest       : {:#018x}\n",
      cfg.topology.k, sc.sim.fabric().host_count(),
      fabric::fat_tree_switch_count(cfg.topology.k), sc.sim.fabric().port_count(),
      fabric::fat_tree_cross_pod_paths(cfg.topology.k), fabric::routing_name(sc.routing), sc.seed,
      sc.flow_count, cfg.workload.flow_bytes, cfg.workload.mtu_bytes, cfg.workload.load_permille,
      fct.p50_ns, fct.p90_ns, fct.p99_ns, fct.max_ns, sc.best_case_ns, stats.bytes_injected,
      stats.bytes_delivered, stats.bytes_dropped, stats.flows_complete, stats.flows_incomplete,
      stats.events_processed, wall_seconds,
      wall_seconds > 0.0 ? static_cast<double>(stats.events_processed) / wall_seconds / 1e6 : 0.0,
      sc.sim.result_digest());

  if (!fct.complete()) {
    // Percentiles over the survivors flatter every one of them, because the
    // flows that were doing worst are exactly the ones missing. Say so rather
    // than printing a number that looks fine.
    std::cout << std::format(
        "\nWARNING: {} of {} flows never completed -- drop-tail losses with no\n"
        "         retransmission (arrives in M2). The percentiles above are over\n"
        "         the {} survivors only and are therefore optimistic.\n",
        fct.flows_incomplete, fct.flows_total, fct.flows_complete);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view scenario = "smoke";
  fabric::PermutationConfig perm;
  perm.topology.k = 8;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    std::string_view value;
    std::uint64_t n = 0;

    if (arg == "--help" || arg == "-h") {
      print_usage();
      return EXIT_SUCCESS;
    }
    if (match_value(arg, "--scenario", value)) {
      scenario = value;
    } else if (match_value(arg, "--k", value)) {
      if (!parse_u64(value, n) || n < 2 || n % 2 != 0 || n > 64) {
        std::cerr << "fabricsim: --k must be an even number in [2, 64]\n";
        return 2;
      }
      perm.topology.k = static_cast<std::uint32_t>(n);
    } else if (match_value(arg, "--routing", value)) {
      if (!fabric::parse_routing(value, perm.routing)) {
        std::cerr << "fabricsim: --routing must be ecmp or static\n";
        return 2;
      }
    } else if (match_value(arg, "--seed", value)) {
      if (!parse_u64(value, n)) {
        std::cerr << "fabricsim: --seed must be a number\n";
        return 2;
      }
      perm.workload.seed = n;
    } else if (match_value(arg, "--bytes", value)) {
      if (!parse_u64(value, n) || n == 0) {
        std::cerr << "fabricsim: --bytes must be positive\n";
        return 2;
      }
      perm.workload.flow_bytes = n;
    } else if (match_value(arg, "--mtu", value)) {
      if (!parse_u64(value, n) || n == 0 || n > fabric::kMaxPacketBytes) {
        std::cerr << "fabricsim: --mtu must be in [1, 9216]\n";
        return 2;
      }
      perm.workload.mtu_bytes = static_cast<std::uint16_t>(n);
    } else if (match_value(arg, "--load", value)) {
      if (!parse_u64(value, n) || n == 0 || n > 1000) {
        std::cerr << "fabricsim: --load must be in [1, 1000] permille\n";
        return 2;
      }
      perm.workload.load_permille = static_cast<std::uint32_t>(n);
    } else {
      std::cerr << "fabricsim: unrecognised argument '" << arg << "'\n";
      print_usage();
      return 2;
    }
  }

  if (scenario == "smoke") {
    return run_smoke();
  }
  if (scenario == "permutation") {
    return run_permutation(perm);
  }

  std::cerr << "fabricsim: unknown scenario '" << scenario << "'\n";
  print_usage();
  return 2;
}
