#pragma once

#include <cstddef>
#include <span>

#include "inputs/inputs.hpp"
#include "routing/graph.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/link_plan.hpp"

// The control graph (Phase 4 spec 4.5): who can pass a message (a report, a flooded fact) to
// whom at an instant. Pure: built from one step's overlaid network.

namespace satsim::failures_app {

/// Node numbering of the control graph, S satellites, C cities, G gateways:
///   satellite i      i
///   dish of city c   S + c
///   gateway g        S + C + g
struct ControlNodes {
  std::size_t satellites = 0;
  std::size_t cities = 0;
  std::size_t gateways = 0;

  [[nodiscard]] std::size_t node_count() const noexcept { return satellites + cities + gateways; }
  [[nodiscard]] std::size_t satellite(std::size_t i) const noexcept { return i; }
  [[nodiscard]] std::size_t dish(std::size_t city) const noexcept { return satellites + city; }
  [[nodiscard]] std::size_t gateway(std::size_t g) const noexcept {
    return satellites + cities + g;
  }
  [[nodiscard]] bool is_satellite(std::size_t node) const noexcept { return node < satellites; }
  [[nodiscard]] bool is_dish(std::size_t node) const noexcept {
    return node >= satellites && node < satellites + cities;
  }
  [[nodiscard]] bool is_gateway(std::size_t node) const noexcept {
    return node >= satellites + cities && node < node_count();
  }

  friend bool operator==(const ControlNodes&, const ControlNodes&) = default;
};

/// The control graph of one step. `links` are the run's links (step.links->states[i] is
/// links[i]'s state); `nodes` gives the counts (step's visible lists must have nodes.cities and
/// nodes.gateways entries, else the missing ones get no edges). Undirected edges, each of
/// delay length_km / kSpeedOfLightVacuumKmPerS + hop_delay_s (as routing_app::GraphBuilder's
/// radio and laser edges):
///  - every link whose state is up (its length);
///  - gateway g to every satellite of step.gateway_visible[g] (its slant range), in both
///    endpoint models;
///  - best case only: dish of city c to every satellite of step.city_visible[c] (its slant
///    range). In the realistic model dishes have no edge here.
/// No edge touches a satellite or gateway marked down in step.satellite_down /
/// step.gateway_down (whatever its state or visible list says). An edge whose delay
/// network::Graph refuses (a length that is not finite) is left out. Edges are added in that order
/// (links in index order, gateways in index order, cities in index order; each list in its
/// order).
[[nodiscard]] network::Graph control_graph(const ControlNodes& nodes,
                                           std::span<const topology::PlannedLink> links,
                                           const routing_app::StepGround& step,
                                           app::EndpointModel model, double hop_delay_s);

}  // namespace satsim::failures_app
