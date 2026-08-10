#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>

#include "fabric/core/scoped_timer.hpp"
#include "fabric/harness/scenario.hpp"

namespace {

constexpr std::string_view kScenarioPrefix = "--scenario=";

void print_usage() {
  std::cout << "fabricsim -- GPU fabric congestion simulator\n"
               "\n"
               "Usage:\n"
               "  fabricsim [--scenario=NAME]\n"
               "\n"
               "Scenarios:\n"
               "  smoke   single flow across a 2-spine / 2-leaf / 8-host fabric (default)\n";
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

}  // namespace

int main(int argc, char** argv) {
  std::string_view scenario = "smoke";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return EXIT_SUCCESS;
    }
    if (arg.starts_with(kScenarioPrefix)) {
      scenario = arg.substr(kScenarioPrefix.size());
    } else if (arg == "--scenario" && i + 1 < argc) {
      scenario = std::string_view{argv[++i]};
    } else {
      std::cerr << "fabricsim: unrecognised argument '" << arg << "'\n";
      print_usage();
      return 2;
    }
  }

  if (scenario == "smoke") {
    return run_smoke();
  }

  std::cerr << "fabricsim: unknown scenario '" << scenario << "'\n";
  print_usage();
  return 2;
}
