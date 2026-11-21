#include "failures/control_graph.hpp"

#include <cstddef>
#include <span>

#include "inputs/inputs.hpp"
#include "routing/graph.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"

namespace satsim::failures_app {

namespace {

bool marked(std::span<const char> marks, std::size_t i) noexcept {
  return i < marks.size() && marks[i] != 0;
}

}  // namespace

network::Graph control_graph(const ControlNodes& nodes,
                             std::span<const topology::PlannedLink> links,
                             const routing_app::StepGround& step, app::EndpointModel model,
                             double hop_delay_s) {
  network::Graph graph(nodes.node_count());
  const auto delay_s = [&](double km) { return km / kSpeedOfLightVacuumKmPerS + hop_delay_s; };
  const auto satellite_ok = [&](std::size_t s) {
    return s < nodes.satellites && !marked(step.satellite_down, s);
  };
  if (step.links != nullptr) {
    for (std::size_t i = 0; i < links.size() && i < step.links->states.size(); ++i) {
      const topology::LinkState& state = step.links->states[i];
      const topology::PlannedLink& link = links[i];
      if (!state.up() || !state.length_km || !satellite_ok(link.satellite_a) ||
          !satellite_ok(link.satellite_b)) {
        continue;
      }
      // Up with a finite length: the delay is accepted.
      static_cast<void>(
          graph.add_undirected_edge(link.satellite_a, link.satellite_b, delay_s(*state.length_km)));
    }
  }
  for (std::size_t g = 0; g < nodes.gateways && g < step.gateway_visible.size(); ++g) {
    if (marked(step.gateway_down, g)) {
      continue;
    }
    for (const topology::VisibleSatellite& v : step.gateway_visible[g]) {
      if (satellite_ok(v.satellite)) {
        static_cast<void>(
            graph.add_undirected_edge(nodes.gateway(g), v.satellite, delay_s(v.slant_range_km)));
      }
    }
  }
  if (model == app::EndpointModel::kBestCase) {
    for (std::size_t c = 0; c < nodes.cities && c < step.city_visible.size(); ++c) {
      for (const topology::VisibleSatellite& v : step.city_visible[c]) {
        if (satellite_ok(v.satellite)) {
          static_cast<void>(
              graph.add_undirected_edge(nodes.dish(c), v.satellite, delay_s(v.slant_range_km)));
        }
      }
    }
  }
  return graph;
}

}  // namespace satsim::failures_app
