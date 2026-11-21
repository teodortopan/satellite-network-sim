#include "satsim/routing/planner.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/network/path.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/strategy.hpp"

namespace satsim::routing {

std::string_view name_of(PlanOutcome outcome) noexcept {
  switch (outcome) {
    case PlanOutcome::kKept:
      return "kept";
    case PlanOutcome::kReplaced:
      return "replaced";
    case PlanOutcome::kNone:
      return "none";
  }
  return "unknown";
}

namespace {

// A route with its delay in the planning graph.
struct Candidate {
  Route route;
  double delay_s = 0.0;
};

// The shortest path from source to target: `given` when the caller has it, else searched.
std::optional<Candidate> shortest(const network::Graph& graph, std::size_t source,
                                  std::size_t target, const Route* given) {
  Route route = given != nullptr ? *given : network::shortest_paths(graph, source).path_to(target);
  if (route.empty()) {
    return std::nullopt;
  }
  const std::optional<double> delay_s = network::path_delay_s(graph, route);
  // A path shortest_paths returned is up by construction.
  if (!delay_s.has_value()) {
    return std::nullopt;
  }
  return Candidate{.route = std::move(route), .delay_s = *delay_s};
}

// Whether head and tail share a node other than the joint head.back() == tail.front().
bool share_a_node_besides_the_joint(const Route& head, const Route& tail) {
  const bool joined = head.back() == tail.front();
  for (std::size_t i = 0; i < head.size(); ++i) {
    for (std::size_t j = 0; j < tail.size(); ++j) {
      const bool is_joint = joined && i + 1 == head.size() && j == 0;
      if (!is_joint && head[i] == tail[j]) {
        return true;
      }
    }
  }
  return false;
}

// The longest-lived choice among the candidates (planner.hpp), or nullopt.
std::optional<Candidate> best_joined(const network::Graph& graph, std::size_t source,
                                     std::size_t target, const AccessCandidates& candidates) {
  std::optional<Candidate> best;
  for (const Route& head : candidates.heads) {
    if (head.empty() || head.front() != source || !network::path_is_up(graph, head)) {
      continue;
    }
    for (const Route& tail : candidates.tails) {
      if (tail.empty() || tail.back() != target || !network::path_is_up(graph, tail) ||
          share_a_node_besides_the_joint(head, tail)) {
        continue;
      }
      Route route = head;
      if (head.back() != tail.front()) {
        // Every node of the head and the tail but the two ends of the middle; this includes
        // the source (head.front()) and the target (tail.back()) unless they are those ends.
        std::vector<std::size_t> blocked(head.begin(), head.end() - 1);
        blocked.insert(blocked.end(), tail.begin() + 1, tail.end());
        const Route middle =
            network::shortest_paths_avoiding(graph, head.back(), blocked).path_to(tail.front());
        if (middle.empty()) {
          continue;
        }
        route.insert(route.end(), middle.begin() + 1, middle.end());
      }
      route.insert(route.end(), tail.begin() + 1, tail.end());
      const std::optional<double> delay_s = network::path_delay_s(graph, route);
      if (!delay_s.has_value()) {
        continue;  // Cannot happen: head, middle and tail are each up.
      }
      if (!best.has_value() || *delay_s < best->delay_s) {
        best = Candidate{.route = std::move(route), .delay_s = *delay_s};
      }
    }
  }
  return best;
}

}  // namespace

PlanResult RoutePlanner::plan(const network::Graph& graph, std::size_t source, std::size_t target,
                              const AccessCandidates& candidates) {
  return plan_from(graph, source, target, nullptr, candidates);
}

PlanResult RoutePlanner::plan_with_shortest(const network::Graph& graph, std::size_t source,
                                            std::size_t target, const Route& shortest_path,
                                            const AccessCandidates& candidates) {
  return plan_from(graph, source, target, &shortest_path, candidates);
}

PlanResult RoutePlanner::plan_from(const network::Graph& graph, std::size_t source,
                                   std::size_t target, const Route* shortest_path,
                                   const AccessCandidates& candidates) {
  std::optional<double> held_s;
  if (!route_.empty() && route_.front() == source && route_.back() == target) {
    held_s = network::path_delay_s(graph, route_);
  }
  const auto keep = [&]() { return PlanResult{.outcome = PlanOutcome::kKept, .delay_s = held_s}; };
  const auto take = [&](std::optional<Candidate> chosen) {
    if (!chosen.has_value()) {
      route_.clear();
      return PlanResult{.outcome = PlanOutcome::kNone, .delay_s = std::nullopt};
    }
    const PlanOutcome outcome =
        chosen->route == route_ ? PlanOutcome::kKept : PlanOutcome::kReplaced;
    route_ = std::move(chosen->route);
    return PlanResult{.outcome = outcome, .delay_s = chosen->delay_s};
  };

  switch (strategy_.kind) {
    case StrategyKind::kLowestDelay:
      return take(shortest(graph, source, target, shortest_path));
    case StrategyKind::kKeepUntilBroken:
      if (held_s.has_value()) {
        return keep();
      }
      return take(shortest(graph, source, target, shortest_path));
    case StrategyKind::kThreshold: {
      std::optional<Candidate> best = shortest(graph, source, target, shortest_path);
      if (held_s.has_value() && best.has_value()) {
        const double limit_s = strategy_.threshold_kind == ThresholdKind::kAbsolute
                                   ? strategy_.threshold_s
                                   : strategy_.threshold_fraction * *held_s;
        if (*held_s - best->delay_s <= limit_s) {  // Exactly at the limit: keep.
          return keep();
        }
      }
      return take(std::move(best));
    }
    case StrategyKind::kLongestLived:
      if (held_s.has_value()) {
        return keep();
      }
      return take(best_joined(graph, source, target, candidates));
  }
  return take(std::nullopt);
}

bool RoutePlanner::held_route_is_up(const network::Graph& step_graph) const noexcept {
  return network::path_is_up(step_graph, route_);
}

std::optional<double> RoutePlanner::held_delay_s(const network::Graph& step_graph) const noexcept {
  return network::path_delay_s(step_graph, route_);
}

}  // namespace satsim::routing
