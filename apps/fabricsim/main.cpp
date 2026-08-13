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
               "  allreduce    ring all-reduce across a k-ary fat-tree\n"
               "\n"
               "Fabric and routing:\n"
               "  --k=N          fat-tree arity, even (default 8 -> 128 hosts)\n"
               "  --routing=X    ecmp | static (default ecmp)\n"
               "  --seed=N       run seed; drives the workload and the ECMP hash (default 0)\n"
               "\n"
               "Traffic:\n"
               "  --bytes=N      bytes per flow, permutation only (default 1000000)\n"
               "  --buffer=N     gradient buffer per GPU, allreduce only (default 1920000)\n"
               "  --gpus=N       ranks in the ring, allreduce only (default: every host)\n"
               "  --placement=X  pods | seq, allreduce only (default pods)\n"
               "  --mtu=N        packet size in bytes (default 1500)\n"
               "  --load=N       offered load per source in permille, 1..1000 (default 1000)\n"
               "\n"
               "Transport:\n"
               "  --window=N     sender window in packets (default 32)\n"
               "  --ack=N        ack size in bytes (default 64)\n"
               "  --rto=N        retransmission timeout in ns (default: sized from topology)\n"
               "  --queue=N      per-port buffer in packets (default 128)\n";
}

struct Options {
  std::uint32_t k = 8;
  fabric::RoutingAlgorithm routing = fabric::RoutingAlgorithm::Ecmp;
  std::uint64_t seed = 0;
  fabric::Bytes flow_bytes = 1'000'000;
  fabric::Bytes buffer_bytes = 1'920'000;
  std::uint32_t gpus = 0;
  fabric::RingPlacement placement = fabric::RingPlacement::RoundRobinPods;
  std::uint16_t mtu_bytes = 1500;
  std::uint32_t load_permille = 1000;
  std::uint32_t window_pkts = 32;
  fabric::Bytes ack_bytes = 64;
  fabric::Nanos rto_ns = 0;
  std::uint32_t queue_pkts = 128;
};

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

void print_tail(const fabric::RunStats& stats, double wall_seconds, std::uint64_t digest) {
  std::cout << std::format(
      "data bytes   : injected {}, delivered {}, dropped {}\n"
      "ack bytes    : injected {}, delivered {}, dropped {}\n"
      "retransmits  : {} packets\n"
      "flows        : {} complete, {} incomplete, {} gave up\n"
      "events       : {} processed in {:.4f} s wall ({:.2f} M events/s)\n"
      "digest       : {:#018x}\n",
      stats.bytes_injected, stats.bytes_delivered, stats.bytes_dropped, stats.ack_bytes_injected,
      stats.ack_bytes_delivered, stats.ack_bytes_dropped, stats.packets_retransmitted,
      stats.flows_complete, stats.flows_incomplete, stats.flows_failed, stats.events_processed,
      wall_seconds,
      wall_seconds > 0.0 ? static_cast<double>(stats.events_processed) / wall_seconds / 1e6 : 0.0,
      digest);
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
      "transport    : window {} pkts, ack {} B, rto {} ns\n"
      "\n"
      "fct simulated: {} ns\n"
      "fct analytic : {} ns{}\n"
      "delta        : {} ns\n"
      "\n",
      cfg.topology.spine_count, cfg.topology.leaf_count, sc.sim.fabric().host_count(),
      sc.sim.fabric().port_count(), cfg.flow_bytes, cfg.mtu_bytes, sc.packets, sc.hops,
      cfg.window_pkts, sc.sim.transport().ack_bytes, sc.sim.transport().rto_ns, r.fct_ns(),
      sc.analytical_ns, sc.analytical_exact ? "" : "  (approximate: see build_smoke)",
      static_cast<std::int64_t>(r.fct_ns()) - static_cast<std::int64_t>(sc.analytical_ns));

  print_tail(stats, wall_seconds, sc.sim.result_digest());
  return (r.complete && stats.bytes_dropped == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run_permutation(const Options& opt) {
  fabric::PermutationConfig cfg;
  cfg.topology.k = opt.k;
  cfg.topology.host_link.queue_capacity_pkts = opt.queue_pkts;
  cfg.topology.fabric_link.queue_capacity_pkts = opt.queue_pkts;
  cfg.routing = opt.routing;
  cfg.workload.seed = opt.seed;
  cfg.workload.flow_bytes = opt.flow_bytes;
  cfg.workload.mtu_bytes = opt.mtu_bytes;
  cfg.workload.load_permille = opt.load_permille;
  cfg.workload.window_pkts = opt.window_pkts;
  cfg.transport.ack_bytes = opt.ack_bytes;
  cfg.transport.rto_ns = opt.rto_ns;

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
      "transport    : window {} pkts, ack {} B, rto {} ns\n"
      "\n"
      "fct p50      : {} ns\n"
      "fct p90      : {} ns\n"
      "fct p99      : {} ns\n"
      "fct max      : {} ns\n"
      "best case    : {} ns  (uncongested lower bound)\n"
      "\n",
      opt.k, sc.sim.fabric().host_count(), fabric::fat_tree_switch_count(opt.k),
      sc.sim.fabric().port_count(), fabric::fat_tree_cross_pod_paths(opt.k),
      fabric::routing_name(sc.routing), sc.seed, sc.flow_count, cfg.workload.flow_bytes,
      cfg.workload.mtu_bytes, cfg.workload.load_permille, opt.window_pkts,
      sc.sim.transport().ack_bytes, sc.sim.transport().rto_ns, fct.p50_ns, fct.p90_ns, fct.p99_ns,
      fct.max_ns, sc.best_case_ns);

  print_tail(stats, wall_seconds, sc.sim.result_digest());

  if (!fct.complete()) {
    // Percentiles over the survivors flatter every one of them, because the
    // flows that were doing worst are exactly the ones missing. Say so rather
    // than printing a number that looks fine.
    std::cout << std::format(
        "\nWARNING: {} of {} flows never completed. The percentiles above are\n"
        "         over the {} survivors only and are therefore optimistic.\n",
        fct.flows_incomplete, fct.flows_total, fct.flows_complete);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

int run_all_reduce(const Options& opt) {
  fabric::AllReduceConfig cfg;
  cfg.topology.k = opt.k;
  cfg.topology.host_link.queue_capacity_pkts = opt.queue_pkts;
  cfg.topology.fabric_link.queue_capacity_pkts = opt.queue_pkts;
  cfg.routing = opt.routing;
  cfg.workload.seed = opt.seed;
  cfg.workload.buffer_bytes = opt.buffer_bytes;
  cfg.workload.mtu_bytes = opt.mtu_bytes;
  cfg.workload.load_permille = opt.load_permille;
  cfg.workload.window_pkts = opt.window_pkts;
  cfg.workload.gpu_count = opt.gpus;
  cfg.workload.placement = opt.placement;
  cfg.transport.ack_bytes = opt.ack_bytes;
  cfg.transport.rto_ns = opt.rto_ns;

  fabric::AllReduceScenario sc = fabric::build_all_reduce(cfg);

  double wall_seconds = 0.0;
  fabric::RunStats stats;
  {
    const fabric::ScopedTimer timer(wall_seconds);
    stats = fabric::run_with_routing(sc.sim, sc.routing, sc.seed);
  }

  const fabric::Nanos measured = fabric::collective_time_ns(sc.sim.results());
  const double overhead =
      sc.analytical_ns > 0 && measured > 0
          ? (static_cast<double>(measured) / static_cast<double>(sc.analytical_ns) - 1.0) * 100.0
          : 0.0;

  std::cout << std::format(
      "topology     : fat-tree k={}, {} hosts, {} switches, {} directed links\n"
      "collective   : ring all-reduce, {} ranks, {} steps, {} B/GPU\n"
      "chunk        : {} bytes ({} packets), {} hops per step{}\n"
      "placement    : {}\n"
      "routing      : {} (seed {})\n"
      "transport    : window {} pkts, ack {} B, rto {} ns\n"
      "\n"
      "collective   : {} ns\n"
      "analytical   : {} ns  ({})\n"
      "over ideal   : {:+.2f} %\n"
      "\n",
      opt.k, sc.sim.fabric().host_count(), fabric::fat_tree_switch_count(opt.k),
      sc.sim.fabric().port_count(), sc.gpus, sc.steps, cfg.workload.buffer_bytes, sc.chunk_bytes,
      sc.chunk_packets, sc.hops, sc.uniform_hops ? "" : " (max; hops are not uniform)",
      opt.placement == fabric::RingPlacement::RoundRobinPods ? "round-robin over pods"
                                                             : "sequential",
      fabric::routing_name(sc.routing), sc.seed, opt.window_pkts, sc.sim.transport().ack_bytes,
      sc.sim.transport().rto_ns, measured, sc.analytical_ns,
      sc.analytical_exact ? "exact identity" : "lower bound", overhead);

  print_tail(stats, wall_seconds, sc.sim.result_digest());

  if (measured == 0) {
    std::cout << std::format(
        "\nWARNING: {} of {} flows never completed, so the collective never\n"
        "         finished. A barrier gives no partial credit.\n",
        stats.flows_incomplete, stats.flows_complete + stats.flows_incomplete);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view scenario = "smoke";
  Options opt;

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
      opt.k = static_cast<std::uint32_t>(n);
    } else if (match_value(arg, "--routing", value)) {
      if (!fabric::parse_routing(value, opt.routing)) {
        std::cerr << "fabricsim: --routing must be ecmp or static\n";
        return 2;
      }
    } else if (match_value(arg, "--placement", value)) {
      if (value == "pods") {
        opt.placement = fabric::RingPlacement::RoundRobinPods;
      } else if (value == "seq") {
        opt.placement = fabric::RingPlacement::Sequential;
      } else {
        std::cerr << "fabricsim: --placement must be pods or seq\n";
        return 2;
      }
    } else if (match_value(arg, "--seed", value)) {
      if (!parse_u64(value, n)) {
        std::cerr << "fabricsim: --seed must be a number\n";
        return 2;
      }
      opt.seed = n;
    } else if (match_value(arg, "--bytes", value)) {
      if (!parse_u64(value, n) || n == 0) {
        std::cerr << "fabricsim: --bytes must be positive\n";
        return 2;
      }
      opt.flow_bytes = n;
    } else if (match_value(arg, "--buffer", value)) {
      if (!parse_u64(value, n) || n == 0) {
        std::cerr << "fabricsim: --buffer must be positive\n";
        return 2;
      }
      opt.buffer_bytes = n;
    } else if (match_value(arg, "--gpus", value)) {
      if (!parse_u64(value, n) || n < 2) {
        std::cerr << "fabricsim: --gpus must be at least 2\n";
        return 2;
      }
      opt.gpus = static_cast<std::uint32_t>(n);
    } else if (match_value(arg, "--mtu", value)) {
      if (!parse_u64(value, n) || n == 0 || n > fabric::kMaxPacketBytes) {
        std::cerr << "fabricsim: --mtu must be in [1, 9216]\n";
        return 2;
      }
      opt.mtu_bytes = static_cast<std::uint16_t>(n);
    } else if (match_value(arg, "--load", value)) {
      if (!parse_u64(value, n) || n == 0 || n > 1000) {
        std::cerr << "fabricsim: --load must be in [1, 1000] permille\n";
        return 2;
      }
      opt.load_permille = static_cast<std::uint32_t>(n);
    } else if (match_value(arg, "--window", value)) {
      if (!parse_u64(value, n) || n == 0) {
        std::cerr << "fabricsim: --window must be positive\n";
        return 2;
      }
      opt.window_pkts = static_cast<std::uint32_t>(n);
    } else if (match_value(arg, "--ack", value)) {
      if (!parse_u64(value, n) || n == 0 || n > fabric::kMaxPacketBytes) {
        std::cerr << "fabricsim: --ack must be in [1, 9216]\n";
        return 2;
      }
      opt.ack_bytes = n;
    } else if (match_value(arg, "--rto", value)) {
      if (!parse_u64(value, n)) {
        std::cerr << "fabricsim: --rto must be a number of nanoseconds\n";
        return 2;
      }
      opt.rto_ns = n;
    } else if (match_value(arg, "--queue", value)) {
      if (!parse_u64(value, n) || n == 0) {
        std::cerr << "fabricsim: --queue must be positive\n";
        return 2;
      }
      opt.queue_pkts = static_cast<std::uint32_t>(n);
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
    return run_permutation(opt);
  }
  if (scenario == "allreduce") {
    return run_all_reduce(opt);
  }

  std::cerr << "fabricsim: unknown scenario '" << scenario << "'\n";
  print_usage();
  return 2;
}
