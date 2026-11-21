#include "routing/engine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/path.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/change.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::routing_app {

namespace {

constexpr double kNsPerS = 1e9;
constexpr double kMsPerS = 1000.0;

double seconds(std::int64_t ns) noexcept { return static_cast<double>(ns) / kNsPerS; }

bool uses_longest_lived(const EngineSettings& settings) {
  return std::any_of(
      settings.strategies.begin(), settings.strategies.end(),
      [](const routing::Strategy& s) { return s.kind == routing::StrategyKind::kLongestLived; });
}

// Whether `marks` marks element i down (an empty or short span marks nothing).
bool marked(std::span<const char> marks, std::size_t i) noexcept {
  return i < marks.size() && marks[i] != 0;
}

bool has_edge(const network::Graph& graph, std::size_t from, std::size_t to) {
  if (from >= graph.node_count()) {
    return false;
  }
  const auto edges = graph.edges_from(from);
  return std::any_of(edges.begin(), edges.end(),
                     [&](const network::Edge& e) { return e.to == to; });
}

// The highest-elevation satellite of `visible` (ties to the lower catalogue number).
std::optional<std::size_t> highest(std::span<const topology::VisibleSatellite> visible) {
  const auto best = topology::pick_serving_satellite(visible);
  return best ? std::optional(best->satellite) : std::nullopt;
}

// Whether a beats b on (time left, elevation); equal is not better, so the earlier satellite
// (lower catalogue number) keeps a tie.
bool longer_lived(double time_a, const topology::VisibleSatellite& a, double time_b,
                  const topology::VisibleSatellite& b) {
  return time_a > time_b || (time_a == time_b && a.elevation_rad > b.elevation_rad);
}

}  // namespace

std::size_t position_steps_needed(const EngineSettings& settings, EndpointModel model) {
  const double step_s = seconds(settings.step_ns);
  std::size_t n = full_steps_needed(settings);
  if (uses_longest_lived(settings)) {
    n = std::max({n, topology::view_steps(step_s, kViewHorizonS), std::size_t{2}});
  }
  if (model == EndpointModel::kRealistic) {
    if (settings.serving == ServingRule::kLongestInView) {
      n = std::max(n, topology::view_steps(step_s, kViewHorizonS));
    }
    if (settings.serving == ServingRule::kHighestWholeSlot) {
      n = std::max(n, topology::view_steps(step_s, seconds(settings.slot_ns)));
    }
  }
  return std::max(n, std::size_t{1});
}

std::size_t full_steps_needed(const EngineSettings& settings) {
  if (settings.planner == Planner::kWholeInterval && settings.step_ns > 0) {
    return static_cast<std::size_t>(settings.replan_ns / settings.step_ns);
  }
  return 1;
}

Engine::Engine(EngineNetwork network, EngineSettings settings, BeforeChecks before_checks)
    : network_(std::move(network)),
      settings_(std::move(settings)),
      before_checks_(std::move(before_checks)),
      builder_(GraphLayout(), *network_.plan, {}, {}, 0.0) {
  const std::size_t d_count = network_.directions.size();
  const std::size_t k_count = settings_.strategies.size();
  replan_steps_ = std::max<std::int64_t>(1, settings_.replan_ns / settings_.step_ns);
  slot_steps_ = std::max<std::int64_t>(1, settings_.slot_ns / settings_.step_ns);

  std::vector<std::size_t> dish_cities;
  if (realistic()) {
    if (settings_.serving == ServingRule::kWithRoute) {
      for (std::size_t d = 0; d < d_count; ++d) {
        for (std::size_t s = 0; s < k_count; ++s) {
          dish_cities.push_back(network_.directions[d].from);
        }
      }
    } else {
      for (std::size_t c = 0; c < network_.city_count; ++c) {
        dish_cities.push_back(c);
      }
    }
  }
  for (const std::size_t city : dish_cities) {
    dishes_.push_back(Dish{.city = city, .serving = std::nullopt});
  }
  const GraphLayout layout(network_.model, network_.satellite_count, network_.city_count,
                           network_.gateway_count, d_count, dish_cities.size());
  builder_ = GraphBuilder(layout, *network_.plan, network_.end_delays_s, std::move(dish_cities),
                          settings_.hop_delay_s);
  step_graph_ = network::Graph(layout.node_count());
  planning_graph_ = network::Graph(layout.node_count());
  view_graph_ = network::Graph(layout.node_count());
  open_graph_ = network::Graph(layout.node_count());

  for (std::size_t i = 0; i < network_.plan->links.size(); ++i) {
    const auto& link = network_.plan->links[i];
    link_index_.emplace(std::minmax(link.satellite_a, link.satellite_b), i);
  }
  lanes_.reserve(d_count * k_count);
  for (std::size_t d = 0; d < d_count; ++d) {
    for (std::size_t s = 0; s < k_count; ++s) {
      lanes_.push_back(Lane{.planner = routing::RoutePlanner(settings_.strategies[s]),
                            .plan_failure = std::nullopt,
                            .plan_failure_responsible = {},
                            .has_previous = false,
                            .previous_route = {},
                            .previous_ok = false,
                            .previous_delay_s = 0.0,
                            .lifetime_steps = 0,
                            .open_event = std::nullopt});
    }
  }
  measures_.assign(d_count * k_count, Measures{});
}

std::size_t Engine::dish_of(std::size_t direction, std::size_t strategy) const {
  return settings_.serving == ServingRule::kWithRoute ? lane_index(direction, strategy)
                                                      : network_.directions[direction].from;
}

std::size_t Engine::source_node(std::size_t direction, std::size_t strategy) const {
  const GraphLayout& l = builder_.layout();
  return realistic() ? l.serving_dish(dish_of(direction, strategy))
                     : l.open_dish(network_.directions[direction].from);
}

std::size_t Engine::target_node(std::size_t direction) const {
  const GraphLayout& l = builder_.layout();
  return realistic() ? l.end(direction) : l.receiving_dish(network_.directions[direction].to);
}

const std::vector<double>& Engine::times_left(const Lookahead& ahead, const StepGround& now,
                                              std::size_t site) {
  auto it = times_left_.find(site);
  if (it == times_left_.end()) {
    const std::size_t c = network_.city_count;
    const auto& list = site < c ? now.city_visible[site] : now.gateway_visible[site - c];
    it = times_left_.emplace(site, ahead.times_left_in_view_s(site, list, kViewHorizonS)).first;
  }
  return it->second;
}

std::optional<std::size_t> Engine::pick_by_rule(const Lookahead& view, const StepGround& seen,
                                                std::size_t city) {
  const std::span<const topology::VisibleSatellite> visible = seen.city_visible[city];
  switch (settings_.serving) {
    case ServingRule::kLongestInView: {
      const std::vector<double>& time = times_left(view, seen, city);
      std::optional<std::size_t> best;
      for (std::size_t i = 0; i < visible.size(); ++i) {
        if (!best || longer_lived(time[i], visible[i], time[*best], visible[*best])) {
          best = i;
        }
      }
      return best ? std::optional(visible[*best].satellite) : std::nullopt;
    }
    case ServingRule::kHighestWholeSlot: {
      const double slot_s = seconds(settings_.slot_ns);
      const std::vector<double> time = view.times_left_in_view_s(city, visible, slot_s);
      std::vector<topology::VisibleSatellite> whole;
      for (std::size_t i = 0; i < visible.size(); ++i) {
        if (time[i] >= slot_s) {
          whole.push_back(visible[i]);
        }
      }
      return whole.empty() ? highest(visible) : highest(whole);
    }
    case ServingRule::kHighestElevation:
    case ServingRule::kWithRoute:
      break;
  }
  return highest(visible);
}

routing::Route Engine::shortest(const network::Graph& graph, std::size_t source, std::size_t target,
                                std::map<std::size_t, network::ShortestPaths>& cache) const {
  auto it = cache.find(source);
  if (it == cache.end()) {
    it = cache.emplace(source, network::shortest_paths(graph, source)).first;
  }
  return it->second.path_to(target);
}

routing::AccessCandidates Engine::candidates(const network::Graph& graph, const Lookahead& ahead,
                                             const StepGround& now, std::size_t d, std::size_t s,
                                             const routing::Route& shortest_route, bool open_head) {
  const GraphLayout& l = builder_.layout();
  const EngineDirection& dir = network_.directions[d];
  const std::size_t source = source_node(d, s);
  const std::size_t target = target_node(d);
  routing::AccessCandidates result;

  // For every shell, the north-moving and the south-moving satellite of `visible` (seen from
  // `site`) with the longest time left in view, among those `usable` accepts: shells in
  // increasing index, north before south. Lasers never join two shells, so one candidate of
  // each direction overall could leave the two ends in shells nothing joins (spec 4.5).
  const auto pick = [&](std::size_t site, std::span<const topology::VisibleSatellite> visible,
                        const auto& usable) {
    const std::vector<double>& time = times_left(ahead, now, site);
    // shell -> [0] north, [1] south.
    std::map<std::size_t, std::array<std::optional<std::size_t>, 2>> best;
    for (std::size_t i = 0; i < visible.size(); ++i) {
      if (!usable(visible[i].satellite)) {
        continue;
      }
      auto& slot =
          best[visible[i].shell_index].at(ahead.moving_north(visible[i].satellite) ? 0 : 1);
      if (!slot || longer_lived(time[i], visible[i], time[*slot], visible[*slot])) {
        slot = i;
      }
    }
    std::vector<std::size_t> chosen;
    for (const auto& [shell, pair] : best) {
      for (const auto& b : pair) {
        if (b) {
          chosen.push_back(visible[*b].satellite);
        }
      }
    }
    return chosen;
  };

  if (!realistic() || open_head) {
    for (const std::size_t v : pick(dir.from, now.city_visible[dir.from], [&](std::size_t sat) {
           return has_edge(graph, source, sat);
         })) {
      result.heads.push_back({source, v});
    }
  } else if (const auto serving = dishes_[dish_of(d, s)].serving;
             serving && has_edge(graph, source, *serving)) {
    result.heads.push_back({source, *serving});
  }

  if (!realistic()) {
    for (const std::size_t v : pick(dir.to, now.city_visible[dir.to], [&](std::size_t sat) {
           return has_edge(graph, sat, target);
         })) {
      result.tails.push_back({v, target});
    }
    return result;
  }
  // Realistic: the gateway of the shortest path at this instant.
  if (shortest_route.size() < 2) {
    return result;
  }
  const NodeMeaning m = l.meaning(shortest_route[shortest_route.size() - 2]);
  if (m.kind != NodeKind::kGateway) {
    return result;
  }
  const std::size_t gateway_node = l.gateway(m.index);
  for (const std::size_t v :
       pick(network_.city_count + m.index, now.gateway_visible[m.index],
            [&](std::size_t sat) { return has_edge(graph, sat, gateway_node); })) {
    result.tails.push_back({v, gateway_node, target});
  }
  return result;
}

std::optional<std::string> Engine::choose_serving(const Lookahead& view, const StepGround& seen) {
  std::vector<bool> used(dishes_.size(), false);
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    for (std::size_t s = 0; s < settings_.strategies.size(); ++s) {
      used[dish_of(d, s)] = true;
    }
  }
  for (std::size_t i = 0; i < dishes_.size(); ++i) {
    dishes_[i].serving = used[i] ? pick_by_rule(view, seen, dishes_[i].city) : std::nullopt;
  }
  if (settings_.serving != ServingRule::kWithRoute) {
    return std::nullopt;
  }
  // With-route: the first satellite of the route each lane's strategy would choose if its dish
  // could use any satellite it sees (a copy of the lane's planner plans on the open graph).
  std::vector<std::optional<std::size_t>> serving(dishes_.size());
  if (auto error = builder_.build(std::span(&seen, 1), serving, 1, true, open_graph_)) {
    return error;
  }
  std::map<std::size_t, network::ShortestPaths> cache;
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    if (!network_.directions[d].has_home_pop) {
      continue;
    }
    for (std::size_t s = 0; s < settings_.strategies.size(); ++s) {
      const std::size_t source = source_node(d, s);
      const std::size_t target = target_node(d);
      routing::RoutePlanner copy = lanes_[lane_index(d, s)].planner;
      const routing::Route sp = shortest(open_graph_, source, target, cache);
      const routing::AccessCandidates c =
          copy.strategy().kind == routing::StrategyKind::kLongestLived
              ? candidates(open_graph_, view, seen, d, s, sp, true)
              : routing::AccessCandidates{};
      (void)copy.plan_with_shortest(open_graph_, source, target, sp, c);
      if (copy.route().size() >= 2) {
        dishes_[dish_of(d, s)].serving = copy.route()[1];
      }
    }
  }
  return std::nullopt;
}

std::optional<std::pair<DropCause, Responsible>> Engine::cause_now(const StepGround& now,
                                                                   std::size_t d,
                                                                   std::size_t s) const {
  const EngineDirection& dir = network_.directions[d];
  const auto city = [](std::size_t c) {
    return Responsible{.satellite = std::nullopt,
                       .link = std::nullopt,
                       .link_reason = std::nullopt,
                       .site = SiteRef{.gateway = false, .index = c}};
  };
  if (realistic() && !dir.has_home_pop) {
    return std::pair{DropCause::kNoService, Responsible{}};
  }
  if (now.city_visible[dir.from].empty()) {
    return std::pair{DropCause::kNoCoverage, city(dir.from)};
  }
  if (!realistic()) {
    if (now.city_visible[dir.to].empty()) {
      return std::pair{DropCause::kNoCoverage, city(dir.to)};
    }
    return std::nullopt;
  }
  const std::optional<std::size_t>& serving = dishes_[dish_of(d, s)].serving;
  if (!serving) {
    return std::pair{DropCause::kNoCoverage, city(dir.from)};
  }
  if (find_visible(now.city_visible[dir.from], *serving) == nullptr) {
    Responsible r = city(dir.from);
    r.satellite = serving;
    return std::pair{DropCause::kServingLeftView, r};
  }
  return std::nullopt;
}

std::optional<std::pair<DropCause, Responsible>> Engine::failure_cause(
    const StepGround& now, std::size_t d, std::size_t s, const routing::Route& route) const {
  const GraphLayout& l = builder_.layout();
  if (realistic() && network_.directions[d].has_home_pop) {
    // The dish's uplink goes to its serving satellite whatever route it holds.
    if (const std::optional<std::size_t>& serving = dishes_[dish_of(d, s)].serving;
        serving && marked(now.satellite_down, *serving)) {
      Responsible r;
      r.satellite = serving;
      return std::pair{DropCause::kFailedSatellite, r};
    }
  }
  for (std::size_t i = 0; i < route.size(); ++i) {
    const std::size_t u = route[i];
    if (u >= l.node_count()) {
      return std::nullopt;  // Not a node: no failure to name (a check counts the route).
    }
    const NodeMeaning mu = l.meaning(u);
    if (mu.kind == NodeKind::kSatellite && marked(now.satellite_down, mu.index)) {
      Responsible r;
      r.satellite = mu.index;
      return std::pair{DropCause::kFailedSatellite, r};
    }
    if (mu.kind == NodeKind::kGateway && marked(now.gateway_down, mu.index)) {
      Responsible r;
      r.site = SiteRef{.gateway = true, .index = mu.index};
      return std::pair{DropCause::kFailedGateway, r};
    }
    if (i + 1 >= route.size() || mu.kind != NodeKind::kSatellite ||
        route[i + 1] >= l.node_count()) {
      continue;
    }
    const NodeMeaning mv = l.meaning(route[i + 1]);
    if (mv.kind != NodeKind::kSatellite || marked(now.satellite_down, mv.index)) {
      continue;  // Not a laser edge, or its far end is the failure (named next).
    }
    const auto it = link_index_.find(std::minmax(mu.index, mv.index));
    if (it != link_index_.end() && now.links != nullptr && it->second < now.links->states.size() &&
        now.links->states[it->second].down_reason == topology::LinkDownReason::kFailed) {
      Responsible r;
      r.link = it->second;
      r.link_reason = topology::LinkDownReason::kFailed;
      return std::pair{DropCause::kFailedLink, r};
    }
  }
  return std::nullopt;
}

std::optional<std::pair<DropCause, Responsible>> Engine::broken_edge_cause(
    const StepGround& now, const network::Graph& graph, const routing::Route& route) const {
  const GraphLayout& l = builder_.layout();
  std::optional<std::pair<DropCause, Responsible>> laser;
  for (std::size_t i = 0; i + 1 < route.size(); ++i) {
    const std::size_t u = route[i];
    const std::size_t v = route[i + 1];
    if (u >= l.node_count() || v >= l.node_count() || has_edge(graph, u, v)) {
      continue;
    }
    const NodeMeaning mu = l.meaning(u);
    const NodeMeaning mv = l.meaning(v);
    if (mu.kind == NodeKind::kSatellite && mv.kind == NodeKind::kSatellite) {
      if (laser) {
        continue;
      }
      const auto it = link_index_.find(std::minmax(u, v));
      if (it == link_index_.end()) {
        continue;  // Not a planned link: no cause (a check counts it).
      }
      laser = std::pair{DropCause::kLaserLinkDown,
                        Responsible{.satellite = std::nullopt,
                                    .link = it->second,
                                    .link_reason = now.links->states[it->second].down_reason,
                                    .site = std::nullopt}};
      continue;
    }
    // A ground edge: the satellite end and the site.
    Responsible r;
    const NodeMeaning site = mu.kind == NodeKind::kSatellite ? mv : mu;
    if (mu.kind == NodeKind::kSatellite) {
      r.satellite = u;
    } else if (mv.kind == NodeKind::kSatellite) {
      r.satellite = v;
    }
    switch (site.kind) {
      case NodeKind::kOpenDish:
      case NodeKind::kReceivingDish:
        r.site = SiteRef{.gateway = false, .index = site.index};
        break;
      case NodeKind::kServingDish:
        r.site = SiteRef{.gateway = false, .index = dishes_.at(site.index).city};
        break;
      case NodeKind::kGateway:
        r.site = SiteRef{.gateway = true, .index = site.index};
        break;
      case NodeKind::kEnd:
      case NodeKind::kSatellite:
        if (mu.kind == NodeKind::kGateway) {
          r.site = SiteRef{.gateway = true, .index = mu.index};
        }
        break;
    }
    return std::pair{DropCause::kGroundLinkLost, r};
  }
  return laser;
}

bool Engine::route_hops_exist(const StepGround& now, const routing::Route& route, std::size_t d,
                              std::size_t s) const {
  const GraphLayout& l = builder_.layout();
  const EngineDirection& dir = network_.directions[d];
  if (route.size() < 2) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < route.size(); ++i) {
    if (route[i] >= l.node_count() || route[i + 1] >= l.node_count()) {
      return false;
    }
    const NodeMeaning a = l.meaning(route[i]);
    const NodeMeaning b = l.meaning(route[i + 1]);
    bool exists = false;
    if (a.kind == NodeKind::kSatellite && b.kind == NodeKind::kSatellite) {
      const auto it = link_index_.find(std::minmax(a.index, b.index));
      exists = it != link_index_.end() && now.links->states[it->second].up();
    } else if (a.kind == NodeKind::kOpenDish && b.kind == NodeKind::kSatellite) {
      exists = !realistic() && a.index == dir.from &&
               find_visible(now.city_visible[a.index], b.index) != nullptr;
    } else if (a.kind == NodeKind::kServingDish && b.kind == NodeKind::kSatellite) {
      const Dish& dish = dishes_.at(a.index);
      exists = realistic() && a.index == dish_of(d, s) && dish.serving == b.index &&
               find_visible(now.city_visible[dish.city], b.index) != nullptr;
    } else if (a.kind == NodeKind::kSatellite && b.kind == NodeKind::kReceivingDish) {
      exists = !realistic() && b.index == dir.to &&
               find_visible(now.city_visible[b.index], a.index) != nullptr;
    } else if (a.kind == NodeKind::kSatellite && b.kind == NodeKind::kGateway) {
      exists = realistic() && find_visible(now.gateway_visible[b.index], a.index) != nullptr;
    } else if (a.kind == NodeKind::kGateway && b.kind == NodeKind::kEnd) {
      exists = realistic() && b.index == d && dir.has_home_pop;
    }
    if (!exists) {
      return false;
    }
  }
  return true;
}

void Engine::plan_all(std::int64_t k, const Lookahead& view, const StepGround& seen, bool same,
                      std::vector<RouteRow>& rows) {
  // The planning graph: the view's step graph (the step graph itself when the view is the
  // truth), or the view's edges of the whole interval.
  const network::Graph* graph = &step_graph_;
  std::map<std::size_t, network::ShortestPaths> own_trees;
  std::map<std::size_t, network::ShortestPaths>* trees = &step_trees_;
  plan_graph_ = PlanGraph::kStep;
  if (settings_.planner == Planner::kWholeInterval) {
    const std::size_t n = std::min(static_cast<std::size_t>(replan_steps_), view.known_steps());
    std::vector<StepGround> steps;
    steps.reserve(n);
    for (std::size_t j = 0; j < n; ++j) {
      steps.push_back(view.step(j));
    }
    const auto rest_of_slot = static_cast<std::size_t>(slot_steps_ - k % slot_steps_);
    if (auto error = builder_.build(steps, serving_list(), rest_of_slot, false, planning_graph_)) {
      plan_error_ = std::move(error);
      return;
    }
    graph = &planning_graph_;
    trees = &own_trees;
    plan_graph_ = PlanGraph::kInterval;
  } else if (!same) {
    if (auto error = builder_.build(std::span(&seen, 1), serving_list(), 1, false, view_graph_)) {
      plan_error_ = std::move(error);
      return;
    }
    graph = &view_graph_;
    trees = &own_trees;
    plan_graph_ = PlanGraph::kView;
  }
  std::map<std::pair<std::size_t, routing::Route>, std::optional<double>> backups;
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    const EngineDirection& dir = network_.directions[d];
    for (std::size_t s = 0; s < settings_.strategies.size(); ++s) {
      Lane& lane = lanes_[lane_index(d, s)];
      Measures& m = measures_[lane_index(d, s)];
      RouteRow& row = rows[lane_index(d, s)];
      const std::size_t source = source_node(d, s);
      const std::size_t target = target_node(d);
      const bool servable = !realistic() || dir.has_home_pop;
      const routing::Route sp =
          servable ? shortest(*graph, source, target, *trees) : routing::Route{};
      const routing::AccessCandidates c =
          lane.planner.strategy().kind == routing::StrategyKind::kLongestLived
              ? candidates(*graph, view, seen, d, s, sp, false)
              : routing::AccessCandidates{};
      const routing::PlanResult result =
          lane.planner.plan_with_shortest(*graph, source, target, sp, c);
      row.plan = result.outcome;
      ++m.plans;
      switch (result.outcome) {
        case routing::PlanOutcome::kKept:
          ++m.kept;
          break;
        case routing::PlanOutcome::kReplaced:
          ++m.replaced;
          break;
        case routing::PlanOutcome::kNone:
          ++m.no_route;
          break;
      }
      lane.plan_failure.reset();
      lane.plan_failure_responsible = {};
      if (result.outcome == routing::PlanOutcome::kNone) {
        if (auto now_cause = cause_now(seen, d, s)) {
          lane.plan_failure = now_cause->first;
          lane.plan_failure_responsible = now_cause->second;
        } else if (realistic()) {
          lane.plan_failure = DropCause::kNoGateway;
          lane.plan_failure_responsible.satellite = dishes_[dish_of(d, s)].serving;
        } else {
          lane.plan_failure = DropCause::kNoPath;
        }
      }
      if (!settings_.backup || !lane.planner.has_route() || !result.delay_s) {
        continue;
      }
      const routing::Route& held = lane.planner.route();
      auto it = backups.find({source, held});
      if (it == backups.end()) {
        std::vector<std::size_t> blocked;
        for (std::size_t i = 0; i < held.size(); ++i) {
          const bool serving_satellite = realistic() && i == 1;
          if (held[i] < network_.satellite_count && !serving_satellite) {
            blocked.push_back(held[i]);
          }
        }
        const routing::Route path =
            network::shortest_paths_avoiding(*graph, source, blocked).path_to(target);
        std::optional<double> delay_s;
        if (!path.empty()) {
          delay_s = network::path_delay_s(*graph, path);
        }
        it = backups.emplace(std::pair{source, held}, delay_s).first;
      }
      row.backup_s = it->second;
      if (const std::optional<double>& backup_s = it->second) {
        m.backup_ms.push_back(*backup_s * kMsPerS);
        m.backup_extra_ms.push_back((*backup_s - result.delay_s.value_or(0.0)) * kMsPerS);
      } else {
        ++m.backup_missing;
      }
    }
  }
}

void Engine::close_event(Lane& lane) {
  if (lane.open_event) {
    ++measures_[lane_index(lane.open_event->direction, lane.open_event->strategy)].drop_events;
    events_.push_back(*lane.open_event);
    lane.open_event.reset();
  }
}

void Engine::record(std::int64_t k, const StepGround& now, RouteRow& row) {
  if (before_checks_) {
    before_checks_(row);
  }
  const std::size_t d = row.direction;
  const std::size_t s = row.strategy;
  Lane& lane = lanes_[lane_index(d, s)];
  Measures& m = measures_[lane_index(d, s)];
  const EngineDirection& dir = network_.directions[d];

  // The route checks of spec 6.1.
  if (row.ok) {
    if (!route_hops_exist(now, row.route, d, s)) {
      ++checks_.ok_route_not_in_graph;
    }
    if (!row.ideal_s || row.delay_s < *row.ideal_s) {
      ++checks_.below_ideal;
    }
    if (row.delay_s < dir.lower_bound_s) {
      ++checks_.below_lower_bound;
    }
  } else if (!row.cause) {
    ++checks_.drop_without_cause;
  }
  if (!row.route.empty()) {
    routing::Route sorted = row.route;
    std::sort(sorted.begin(), sorted.end());
    if (row.route.front() != source_node(d, s) || row.route.back() != target_node(d) ||
        std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      ++checks_.bad_route_shape;
    }
  }

  // Measures.
  ++m.steps;
  if (row.ok) {
    ++m.ok_steps;
    m.delay_ms.push_back(row.delay_s * kMsPerS);
    if (row.ideal_s) {
      m.extra_ms.push_back((row.delay_s - *row.ideal_s) * kMsPerS);
    }
  } else if (row.cause) {
    ++m.drop_steps_by_cause.at(static_cast<std::size_t>(*row.cause));
  }
  if (lane.has_previous) {
    const routing::Change change = routing::classify_change(
        {.route = lane.previous_route, .up = lane.previous_ok, .delay_s = lane.previous_delay_s},
        {.route = row.route, .up = row.ok, .delay_s = row.delay_s});
    switch (change.kind) {
      case routing::ChangeKind::kBadChange:
        ++m.bad_changes;
        m.change_sizes_ms.push_back(change.size_s.value_or(0.0) * kMsPerS);
        break;
      case routing::ChangeKind::kDelayIncreaseChange:
        ++m.increase_changes;
        m.change_sizes_ms.push_back(change.size_s.value_or(0.0) * kMsPerS);
        break;
      case routing::ChangeKind::kBreak:
        ++m.breaks;
        break;
      case routing::ChangeKind::kRecover:
        ++m.recoveries;
        break;
      case routing::ChangeKind::kNone:
      case routing::ChangeKind::kStillDown:
        break;
    }
  }
  const double step_s = seconds(settings_.step_ns);
  if (row.ok) {
    if (lane.has_previous && lane.previous_ok && lane.previous_route == row.route) {
      ++lane.lifetime_steps;
    } else {
      if (lane.lifetime_steps > 0) {
        m.lifetimes_s.push_back(static_cast<double>(lane.lifetime_steps) * step_s);
      }
      lane.lifetime_steps = 1;
    }
  } else {
    if (lane.lifetime_steps > 0) {
      m.lifetimes_s.push_back(static_cast<double>(lane.lifetime_steps) * step_s);
    }
    lane.lifetime_steps = 0;
  }
  if (!row.ok && row.cause) {
    if (lane.open_event && lane.open_event->cause == *row.cause) {
      ++lane.open_event->steps;
    } else {
      close_event(lane);
      lane.open_event = DropEvent{.start_step = k,
                                  .steps = 1,
                                  .direction = d,
                                  .strategy = s,
                                  .cause = *row.cause,
                                  .responsible = row.responsible};
    }
  } else {
    close_event(lane);
  }
  lane.previous_route = row.route;
  lane.previous_ok = row.ok;
  lane.previous_delay_s = row.delay_s;
  lane.has_previous = true;
}

std::vector<std::optional<std::size_t>> Engine::serving_list() const {
  std::vector<std::optional<std::size_t>> serving;
  serving.reserve(dishes_.size());
  for (const Dish& dish : dishes_) {
    serving.push_back(dish.serving);
  }
  return serving;
}

LaneState Engine::evaluate(std::size_t d, std::size_t s, const StepGround& truth_now,
                           const network::Graph& truth_graph) const {
  const Lane& lane = lanes_.at(lane_index(d, s));
  LaneState state;
  if (const auto delay_s = lane.planner.held_delay_s(truth_graph)) {
    state.ok = true;
    state.delay_s = delay_s;
    return state;
  }
  const routing::Route& route = lane.planner.route();
  std::optional<std::pair<DropCause, Responsible>> cause = failure_cause(truth_now, d, s, route);
  if (!cause) {
    cause = cause_now(truth_now, d, s);
  }
  if (!cause && !route.empty()) {
    cause = broken_edge_cause(truth_now, truth_graph, route);
  }
  if (!cause && route.empty() && lane.plan_failure) {
    cause = std::pair{*lane.plan_failure, lane.plan_failure_responsible};
  }
  if (cause) {
    state.cause = cause->first;
    state.responsible = cause->second;
  }
  return state;
}

std::optional<std::string> Engine::step(std::int64_t k, const Lookahead& ahead,
                                        std::vector<RouteRow>& rows) {
  return step(k, ahead, ahead, rows);
}

std::optional<std::string> Engine::step(std::int64_t k, const Lookahead& truth,
                                        const Lookahead& view, std::vector<RouteRow>& rows) {
  const bool same = &truth == &view;
  const StepGround now = truth.step(0);
  const StepGround seen = same ? now : view.step(0);
  times_left_.clear();
  step_trees_.clear();
  plan_error_.reset();
  if (realistic() && k % slot_steps_ == 0) {
    if (auto error = choose_serving(view, seen)) {
      return error;
    }
  }
  if (auto error = builder_.build(std::span(&now, 1), serving_list(), 1, false, step_graph_)) {
    return error;
  }

  const GraphLayout& l = builder_.layout();
  const std::size_t k_count = settings_.strategies.size();
  rows.assign(network_.directions.size() * k_count, RouteRow{});
  std::vector<std::optional<double>> ideal(network_.directions.size());
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    const EngineDirection& dir = network_.directions[d];
    const std::size_t open = l.open_dish(dir.from);
    auto it = step_trees_.find(open);
    if (it == step_trees_.end()) {
      it = step_trees_.emplace(open, network::shortest_paths(step_graph_, open)).first;
    }
    if (it->second.reachable(target_node(d))) {
      ideal[d] = it->second.delay_s[target_node(d)];
    }
    for (std::size_t s = 0; s < k_count; ++s) {
      RouteRow& row = rows[lane_index(d, s)];
      row.direction = d;
      row.strategy = s;
      row.ideal_s = ideal[d];
      if (realistic()) {
        row.serving = dishes_[dish_of(d, s)].serving;
      }
    }
  }
  if (k % replan_steps_ == 0) {
    plan_all(k, view, seen, same, rows);
    if (plan_error_) {
      return plan_error_;
    }
  }
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    for (std::size_t s = 0; s < k_count; ++s) {
      RouteRow& row = rows[lane_index(d, s)];
      row.route = lanes_[lane_index(d, s)].planner.route();
      const LaneState state = evaluate(d, s, now, step_graph_);
      if (state.ok) {
        row.ok = true;
        row.delay_s = state.delay_s.value_or(0.0);
      } else if (state.cause) {
        row.cause = state.cause;
        row.responsible = state.responsible;
      }
      record(k, now, row);
    }
  }
  return std::nullopt;
}

std::optional<std::string> Engine::replan(std::int64_t k, const Lookahead& truth,
                                          const Lookahead& view, std::vector<RouteRow>& rows) {
  const bool same = &truth == &view;
  const StepGround now = truth.step(0);
  const StepGround seen = same ? now : view.step(0);
  times_left_.clear();
  step_trees_.clear();
  plan_error_.reset();
  if (auto error = builder_.build(std::span(&now, 1), serving_list(), 1, false, step_graph_)) {
    return error;
  }
  const std::size_t k_count = settings_.strategies.size();
  rows.assign(network_.directions.size() * k_count, RouteRow{});
  for (std::size_t d = 0; d < network_.directions.size(); ++d) {
    for (std::size_t s = 0; s < k_count; ++s) {
      RouteRow& row = rows[lane_index(d, s)];
      row.direction = d;
      row.strategy = s;
      if (realistic()) {
        row.serving = dishes_[dish_of(d, s)].serving;
      }
    }
  }
  plan_all(k, view, seen, same, rows);
  if (plan_error_) {
    return plan_error_;
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i].route = lanes_[i].planner.route();
  }
  return std::nullopt;
}

const network::Graph& Engine::planning_graph() const noexcept {
  switch (plan_graph_) {
    case PlanGraph::kView:
      return view_graph_;
    case PlanGraph::kInterval:
      return planning_graph_;
    case PlanGraph::kStep:
      break;
  }
  return step_graph_;
}

const routing::Route& Engine::lane_route(std::size_t d, std::size_t s) const {
  return lanes_.at(lane_index(d, s)).planner.route();
}

void Engine::hold_route(std::size_t d, std::size_t s, routing::Route route) {
  Lane& lane = lanes_.at(lane_index(d, s));
  lane.planner.hold(std::move(route));
  if (lane.planner.has_route()) {
    lane.plan_failure.reset();
    lane.plan_failure_responsible = {};
  }
}

std::optional<std::size_t> Engine::serving_of(std::size_t dish) const {
  return dishes_.at(dish).serving;
}

void Engine::set_serving(std::size_t dish, std::optional<std::size_t> satellite) {
  dishes_.at(dish).serving = satellite;
}

void Engine::finish() {
  const double step_s = seconds(settings_.step_ns);
  for (std::size_t i = 0; i < lanes_.size(); ++i) {
    Lane& lane = lanes_[i];
    close_event(lane);
    if (lane.lifetime_steps > 0) {
      measures_[i].lifetimes_s.push_back(static_cast<double>(lane.lifetime_steps) * step_s);
      lane.lifetime_steps = 0;
    }
  }
}

}  // namespace satsim::routing_app
