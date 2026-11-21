#include "topology/endpoints.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::topology_app {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double light_delay_s(double length_km) noexcept { return length_km / kSpeedOfLightVacuumKmPerS; }

// The slant range from a site to `satellite`, from the site's visible list. Precondition: the
// satellite is in the list (it is: the graph's ground edges come from the same lists).
double slant_km(std::span<const topology::VisibleSatellite> visible, std::size_t satellite) {
  for (const topology::VisibleSatellite& v : visible) {
    if (v.satellite == satellite) {
      return v.slant_range_km;
    }
  }
  assert(false && "satellite not in the visible list");
  return kNaN;
}

}  // namespace

std::string_view name_of(PathStatus status) noexcept {
  switch (status) {
    case PathStatus::kOk:
      return "ok";
    case PathStatus::kNoService:
      return "no_service";
    case PathStatus::kNoCoverage:
      return "no_coverage";
    case PathStatus::kNoGateway:
      return "no_gateway";
    case PathStatus::kNoPath:
      return "no_path";
  }
  return "unknown";
}

StepNetwork::StepNetwork(std::size_t satellite_count, std::size_t city_count)
    : satellite_count_(satellite_count),
      city_count_(city_count),
      graph_(satellite_count + 2 * city_count) {}

std::optional<std::string> StepNetwork::build(
    const topology::LinkPlan& plan, const topology::StepLinks& links,
    std::span<const std::vector<topology::VisibleSatellite>> city_visible) {
  graph_.clear_edges();
  if (links.states.size() != plan.links.size() || city_visible.size() != city_count_) {
    return "the step's link states or ground links do not match the plan or the cities";
  }
  for (std::size_t i = 0; i < plan.links.size(); ++i) {
    const topology::LinkState& state = links.states[i];
    if (!state.up()) {
      continue;
    }
    const topology::PlannedLink& link = plan.links[i];
    const double delay_s = light_delay_s(state.length_km.value_or(kNaN));
    if (const auto error =
            graph_.add_undirected_edge(link.satellite_a, link.satellite_b, delay_s)) {
      return "laser link " + std::to_string(i) + ": " + std::string(network::name_of(*error));
    }
  }
  for (std::size_t c = 0; c < city_count_; ++c) {
    for (const topology::VisibleSatellite& v : city_visible[c]) {
      const double delay_s = light_delay_s(v.slant_range_km);
      auto error = graph_.add_edge(sending_node(c), v.satellite, delay_s);
      if (!error) {
        error = graph_.add_edge(v.satellite, receiving_node(c), delay_s);
      }
      if (error) {
        return "ground link of city " + std::to_string(c) + ": " +
               std::string(network::name_of(*error));
      }
    }
  }
  return std::nullopt;
}

EndpointEvaluator::EndpointEvaluator(std::vector<ground::GroundSite> cities,
                                     std::vector<std::optional<ground::GroundSite>> home_pops,
                                     std::vector<GatewaySite> gateways,
                                     std::vector<Direction> directions, std::size_t satellite_count,
                                     const EndpointSettings& settings)
    : cities_(std::move(cities)),
      home_pops_(std::move(home_pops)),
      gateways_(std::move(gateways)),
      directions_(std::move(directions)),
      settings_(settings),
      network_(satellite_count, cities_.size()) {
  assert(home_pops_.size() == cities_.size());
  gateway_to_pop_km_.assign(cities_.size() * gateways_.size(), kNaN);
  for (std::size_t c = 0; c < cities_.size(); ++c) {
    if (!home_pops_[c]) {
      continue;
    }
    for (std::size_t g = 0; g < gateways_.size(); ++g) {
      gateway_to_pop_km_[c * gateways_.size() + g] =
          ground::great_circle_distance_on_mean_sphere_km(gateways_[g].site, *home_pops_[c]);
    }
  }
  pop_to_destination_km_.reserve(directions_.size());
  for (const Direction& d : directions_) {
    assert(d.from < cities_.size() && d.to < cities_.size());
    pop_to_destination_km_.push_back(
        home_pops_[d.from]
            ? ground::great_circle_distance_on_mean_sphere_km(*home_pops_[d.from], cities_[d.to])
            : kNaN);
  }
}

std::optional<std::string> EndpointEvaluator::evaluate(
    const topology::LinkPlan& plan, const topology::StepLinks& links,
    std::span<const std::vector<topology::VisibleSatellite>> city_visible,
    std::span<const std::vector<topology::VisibleSatellite>> gateway_visible,
    std::vector<PathResult>& results) {
  results.assign(directions_.size(), PathResult{});
  if (gateway_visible.size() != gateways_.size()) {
    return "the step's gateway links do not match the gateways";
  }
  if (auto error = network_.build(plan, links, city_visible)) {
    return error;
  }
  // One Dijkstra per sending city (best case: from its sending node; realistic: from its
  // serving satellite), shared by every direction from that city.
  std::vector<std::optional<network::ShortestPaths>> from_city(cities_.size());
  for (std::size_t i = 0; i < directions_.size(); ++i) {
    const Direction& d = directions_[i];
    std::optional<network::ShortestPaths>& paths = from_city[d.from];
    if (settings_.model == EndpointModel::kBestCase) {
      if (!paths) {
        paths.emplace(network::shortest_paths(network_.graph(), network_.sending_node(d.from)));
      }
      results[i] = best_case(d, *paths, city_visible);
      continue;
    }
    const std::optional<topology::VisibleSatellite> serving =
        topology::pick_serving_satellite(city_visible[d.from], settings_.serving_rule);
    if (home_pops_[d.from] && serving && !paths) {
      paths.emplace(network::shortest_paths(network_.graph(), serving->satellite));
    }
    results[i] = realistic(i, serving, paths ? &*paths : nullptr, gateway_visible);
  }
  return std::nullopt;
}

PathResult EndpointEvaluator::best_case(
    const Direction& d, const network::ShortestPaths& from_a,
    std::span<const std::vector<topology::VisibleSatellite>> city_visible) const {
  PathResult result;
  if (city_visible[d.from].empty() || city_visible[d.to].empty()) {
    result.status = PathStatus::kNoCoverage;
    return result;
  }
  const std::size_t target = network_.receiving_node(d.to);
  if (!from_a.reachable(target)) {
    result.status = PathStatus::kNoPath;
    return result;
  }
  // sending node, first satellite, ..., last satellite, receiving node.
  const std::vector<std::size_t> path = from_a.path_to(target);
  assert(path.size() >= 3);
  const std::size_t first = path[1];
  const std::size_t last = path[path.size() - 2];
  result.status = PathStatus::kOk;
  result.one_way_s = from_a.delay_s[target];
  result.laser_hops = path.size() - 3;
  result.uplink_km = slant_km(city_visible[d.from], first);
  result.downlink_km = slant_km(city_visible[d.to], last);
  result.laser_km = (from_a.delay_s[last] - from_a.delay_s[first]) * kSpeedOfLightVacuumKmPerS;
  result.first_satellite = first;
  result.last_satellite = last;
  return result;
}

PathResult EndpointEvaluator::realistic(
    std::size_t direction, const std::optional<topology::VisibleSatellite>& serving,
    const network::ShortestPaths* from_serving,
    std::span<const std::vector<topology::VisibleSatellite>> gateway_visible) const {
  const Direction& d = directions_[direction];
  PathResult result;
  if (!home_pops_[d.from]) {
    result.status = PathStatus::kNoService;
    return result;
  }
  if (!serving || from_serving == nullptr) {
    result.status = PathStatus::kNoCoverage;
    return result;
  }
  result.serving_satellite = serving->satellite;

  // The best (cost, laser hops, gateway, satellite) over every gateway's visible satellites.
  struct Best {
    double cost_s = std::numeric_limits<double>::infinity();
    std::size_t hops = 0;
    std::size_t gateway = 0;
    std::size_t satellite = 0;
    double downlink_km = 0.0;
  };
  std::optional<Best> best;
  const std::size_t gateway_count = gateways_.size();
  for (std::size_t g = 0; g < gateway_count; ++g) {
    const double fibre_s = ground::fibre_delay_s(gateway_to_pop_km_[d.from * gateway_count + g],
                                                 settings_.fibre_factor);
    for (const topology::VisibleSatellite& v : gateway_visible[g]) {
      if (!from_serving->reachable(v.satellite)) {
        continue;
      }
      const double cost_s =
          from_serving->delay_s[v.satellite] + light_delay_s(v.slant_range_km) + fibre_s;
      const std::size_t hops = from_serving->hops[v.satellite];
      if (!best || std::tie(cost_s, hops, g, v.satellite) <
                       std::tie(best->cost_s, best->hops, best->gateway, best->satellite)) {
        best = Best{.cost_s = cost_s,
                    .hops = hops,
                    .gateway = g,
                    .satellite = v.satellite,
                    .downlink_km = v.slant_range_km};
      }
    }
  }
  if (!best) {
    result.status = PathStatus::kNoGateway;
    return result;
  }
  const double pop_to_destination_km = pop_to_destination_km_[direction];
  result.status = PathStatus::kOk;
  result.one_way_s = light_delay_s(serving->slant_range_km) + best->cost_s +
                     ground::fibre_delay_s(pop_to_destination_km, settings_.fibre_factor);
  result.laser_hops = best->hops;
  result.uplink_km = serving->slant_range_km;
  result.laser_km = from_serving->delay_s[best->satellite] * kSpeedOfLightVacuumKmPerS;
  result.downlink_km = best->downlink_km;
  result.fibre_km =
      gateway_to_pop_km_[d.from * gateway_count + best->gateway] + pop_to_destination_km;
  result.gateway_satellite = best->satellite;
  result.gateway = best->gateway;
  return result;
}

}  // namespace satsim::topology_app
