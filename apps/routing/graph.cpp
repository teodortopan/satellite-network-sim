#include "routing/graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "satsim/core/physical_constants.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::routing_app {

namespace {

bool realistic(const GraphLayout& layout) noexcept {
  return layout.model() == EndpointModel::kRealistic;
}

// Whether `satellite` is in `visible` at every step of `steps` after the first.
bool seen_at_every_later_step(std::span<const StepGround> steps, bool gateway, std::size_t site,
                              std::size_t satellite, std::size_t step_count) {
  for (std::size_t j = 1; j < step_count; ++j) {
    const auto& lists = gateway ? steps[j].gateway_visible : steps[j].city_visible;
    if (find_visible(lists[site], satellite) == nullptr) {
      return false;
    }
  }
  return true;
}

std::string refused(const std::string& what, network::EdgeError error) {
  return what + ": " + std::string(network::name_of(error));
}

}  // namespace

GraphLayout::GraphLayout(EndpointModel model, std::size_t satellites, std::size_t cities,
                         std::size_t gateways, std::size_t directions,
                         std::size_t serving_dishes) noexcept
    : model_(model),
      satellites_(satellites),
      cities_(cities),
      gateways_(model == EndpointModel::kRealistic ? gateways : 0),
      directions_(model == EndpointModel::kRealistic ? directions : 0),
      serving_dishes_(model == EndpointModel::kRealistic ? serving_dishes : 0) {}

std::size_t GraphLayout::node_count() const noexcept {
  return satellites_ + 2 * cities_ + gateways_ + directions_ + serving_dishes_;
}

NodeMeaning GraphLayout::meaning(std::size_t node) const noexcept {
  if (node < satellites_) {
    return {.kind = NodeKind::kSatellite, .index = node};
  }
  std::size_t rest = node - satellites_;
  if (rest < 2 * cities_) {
    return {.kind = rest % 2 == 0 ? NodeKind::kOpenDish : NodeKind::kReceivingDish,
            .index = rest / 2};
  }
  rest -= 2 * cities_;
  if (rest < gateways_) {
    return {.kind = NodeKind::kGateway, .index = rest};
  }
  rest -= gateways_;
  if (rest < directions_) {
    return {.kind = NodeKind::kEnd, .index = rest};
  }
  return {.kind = NodeKind::kServingDish, .index = rest - directions_};
}

const topology::VisibleSatellite* find_visible(std::span<const topology::VisibleSatellite> visible,
                                               std::size_t satellite) noexcept {
  const auto it = std::lower_bound(
      visible.begin(), visible.end(), satellite,
      [](const topology::VisibleSatellite& v, std::size_t s) { return v.satellite < s; });
  return it != visible.end() && it->satellite == satellite ? &*it : nullptr;
}

GraphBuilder::GraphBuilder(GraphLayout layout, const topology::LinkPlan& plan,
                           std::vector<double> end_delays_s, std::vector<std::size_t> dish_cities,
                           double hop_delay_s)
    : layout_(layout),
      plan_(&plan),
      end_delays_s_(std::move(end_delays_s)),
      dish_cities_(std::move(dish_cities)),
      hop_delay_s_(hop_delay_s) {}

std::optional<std::string> GraphBuilder::build(std::span<const StepGround> steps,
                                               std::span<const std::optional<std::size_t>> serving,
                                               std::size_t serving_steps, bool serving_dishes_open,
                                               network::Graph& graph) const {
  const GraphLayout& l = layout_;
  if (graph.node_count() != l.node_count()) {
    return std::string("the graph has the wrong number of nodes");
  }
  graph.clear_edges();
  if (steps.empty()) {
    return std::string("no step to build a graph from");
  }
  const bool real = realistic(l);
  for (const StepGround& step : steps) {
    if (step.links == nullptr || step.links->states.size() != plan_->links.size() ||
        step.city_visible.size() != l.city_count() ||
        (real && step.gateway_visible.size() != l.gateway_count())) {
      return std::string("the step's link states or ground links do not match the plan or sites");
    }
  }
  if (real &&
      (end_delays_s_.size() != l.direction_count() * l.gateway_count() ||
       dish_cities_.size() != l.serving_dish_count() || serving.size() != l.serving_dish_count())) {
    return std::string("the end delays, dishes or serving satellites do not match the layout");
  }
  const std::size_t n = steps.size();
  const StepGround& first = steps.front();
  const auto radio_s = [&](double km) { return km / kSpeedOfLightVacuumKmPerS + hop_delay_s_; };

  for (std::size_t i = 0; i < plan_->links.size(); ++i) {
    const bool up_throughout = std::all_of(
        steps.begin(), steps.end(), [&](const StepGround& s) { return s.links->states[i].up(); });
    if (!up_throughout) {
      continue;
    }
    const topology::PlannedLink& link = plan_->links[i];
    const double km = first.links->states[i].length_km.value_or(std::nan(""));
    if (const auto error =
            graph.add_undirected_edge(link.satellite_a, link.satellite_b, radio_s(km))) {
      return refused("laser link " + std::to_string(i), *error);
    }
  }
  for (std::size_t c = 0; c < l.city_count(); ++c) {
    for (const topology::VisibleSatellite& v : first.city_visible[c]) {
      if (!seen_at_every_later_step(steps, false, c, v.satellite, n)) {
        continue;
      }
      const double delay_s = radio_s(v.slant_range_km);
      auto error = graph.add_edge(l.open_dish(c), v.satellite, delay_s);
      if (!error && !real) {
        error = graph.add_edge(v.satellite, l.receiving_dish(c), delay_s);
      }
      if (error) {
        return refused("ground link of city " + std::to_string(c), *error);
      }
    }
  }
  if (!real) {
    return std::nullopt;
  }
  for (std::size_t g = 0; g < l.gateway_count(); ++g) {
    for (const topology::VisibleSatellite& v : first.gateway_visible[g]) {
      if (!seen_at_every_later_step(steps, true, g, v.satellite, n)) {
        continue;
      }
      if (const auto error = graph.add_edge(v.satellite, l.gateway(g), radio_s(v.slant_range_km))) {
        return refused("ground link of gateway " + std::to_string(g), *error);
      }
    }
    for (std::size_t d = 0; d < l.direction_count(); ++d) {
      const double fibre_s = end_delays_s_[d * l.gateway_count() + g];
      if (std::isnan(fibre_s)) {
        continue;
      }
      if (const auto error = graph.add_edge(l.gateway(g), l.end(d), fibre_s)) {
        return refused("fibre of gateway " + std::to_string(g), *error);
      }
    }
  }
  for (std::size_t i = 0; i < l.serving_dish_count(); ++i) {
    const std::size_t city = dish_cities_[i];
    for (const topology::VisibleSatellite& v : first.city_visible[city]) {
      if (!serving_dishes_open && serving[i] != v.satellite) {
        continue;
      }
      if (!seen_at_every_later_step(steps, false, city, v.satellite, std::min(n, serving_steps))) {
        continue;
      }
      if (const auto error =
              graph.add_edge(l.serving_dish(i), v.satellite, radio_s(v.slant_range_km))) {
        return refused("serving dish " + std::to_string(i), *error);
      }
    }
  }
  return std::nullopt;
}

}  // namespace satsim::routing_app
