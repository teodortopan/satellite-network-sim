#include "failures/reports.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "failures/control_graph.hpp"
#include "failures/geometry.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/many_sources.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app {

std::vector<FactReports> fact_reports(const Timeline& timeline, const TimelineSettings& settings,
                                      Geometry& geometry) {
  std::map<double, network::ManySourcePaths> paths;
  const auto paths_at = [&](double t) -> const network::ManySourcePaths& {
    auto it = paths.find(t);
    if (it == paths.end()) {
      const std::int64_t k = step_of(t, settings.step_s);
      const topology::StepLinks geometric = geometric_states(geometry, timeline.links, k);
      const OverlaidStep truth = overlay_truth(geometry.at(k), geometric, timeline, t);
      const network::Graph graph = control_graph(timeline.nodes, timeline.links, truth.ground(),
                                                 settings.model, settings.hop_delay_s);
      std::vector<network::Start> starts;
      for (std::size_t g = 0; g < timeline.nodes.gateways; ++g) {
        if (g >= truth.gateway_down.size() || truth.gateway_down[g] == 0) {
          starts.push_back({.node = timeline.nodes.gateway(g), .delay_s = 0.0});
        }
      }
      it = paths
               .emplace(t, network::shortest_paths_from_many(graph, starts,
                                                             settings.times.hop_processing_s))
               .first;
    }
    return it->second;
  };
  const double end_s = static_cast<double>(settings.step_count - 1) * settings.step_s;
  std::vector<FactReports> result;
  result.reserve(timeline.facts.size());
  for (const Fact& fact : timeline.facts) {
    FactReports r;
    if (!fact.old) {
      double first_s = std::numeric_limits<double>::infinity();
      for (const Detection& d : fact.detectors) {
        double arrival_s = 0.0;
        std::size_t hops = 0;
        if (d.t_s > end_s) {
          continue;  // Detected after the run's end: no report.
        }
        if (timeline.nodes.is_gateway(d.node)) {
          arrival_s = d.t_s;
        } else {
          const network::ManySourcePaths& p = paths_at(d.t_s);
          if (!p.reachable(d.node)) {
            continue;
          }
          arrival_s = d.t_s + p.delay_s[d.node];
          hops = p.hops[d.node];
        }
        ++r.reports;
        r.hops_total += hops;
        if (arrival_s < first_s || (arrival_s == first_s && r.first_hops && hops < *r.first_hops)) {
          first_s = arrival_s;
          r.first_hops = hops;
        }
      }
    }
    result.push_back(r);
  }
  return result;
}

}  // namespace satsim::failures_app
