#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "satsim/network/graph.hpp"
#include "satsim/routing/strategy.hpp"

namespace satsim::routing {

/// A route: the nodes traffic passes through, from the source to the target, both included
/// (spec 2). Empty means no route.
using Route = std::vector<std::size_t>;

/// The access choices a longest-lived plan may join (spec 4.5). Ignored by every other
/// strategy.
///
/// - Each head starts at the source and ends at a first satellite, e.g. [dish, satellite].
/// - Each tail starts at a last satellite and ends at the target, e.g. [satellite, dish] or
///   [satellite, gateway, end].
///
/// Their order matters: among candidate routes of equal delay the earlier head wins, then the
/// earlier tail.
struct AccessCandidates {
  std::vector<Route> heads;
  std::vector<Route> tails;
};

/// What a plan did.
enum class PlanOutcome : std::uint8_t {
  kKept,      ///< A route, the same node list as the one held before the plan.
  kReplaced,  ///< A route that differs from the one held before (or there was none).
  kNone,      ///< No route: the direction has none until the next plan.
};

/// "kept", "replaced", "none".
[[nodiscard]] std::string_view name_of(PlanOutcome outcome) noexcept;

struct PlanResult {
  PlanOutcome outcome = PlanOutcome::kNone;
  /// The held route's delay in the planning graph, seconds (network::path_delay_s); nullopt
  /// exactly when outcome is kNone. The route itself is RoutePlanner::route().
  std::optional<double> delay_s;
};

/// The state of one direction under one strategy: the held route, possibly none (spec 4.3,
/// 4.5). `plan` is called at each re-plan instant; between plans the route is held and each
/// step is checked with `held_route_is_up` and `held_delay_s`.
///
/// "Up" is network::path_is_up: every consecutive pair of the route has a directed edge in
/// the graph. "The shortest path" is network::shortest_paths from the source, with its tie
/// rule, so every result is deterministic. Delays are network::path_delay_s, summed in route
/// order. Rules at a plan, with the planning graph:
///
/// - lowest-delay: the shortest path from source to target; none if the target is
///   unreachable.
/// - keep-until-broken: the held route if it is usable (below); otherwise as lowest-delay.
/// - threshold: the shortest path S; keep the held route H if it is usable and
///   delay(H) - delay(S) <= limit, where the limit is threshold_s (absolute) or
///   threshold_fraction × delay(H) (fraction); otherwise S. Exactly at the limit: keep.
/// - longest-lived: the held route if it is usable. Otherwise, over every head × tail of
///   `candidates`, heads in the outer loop: skip the pair if the head is not up or does not
///   start at the source, if the tail is not up or does not end at the target, or if the head
///   and tail share a node other than head.back() == tail.front(). Otherwise join them with
///   the shortest path from head.back() to tail.front() that never enters the source, the
///   target or any other node of the head or tail (network::shortest_paths_avoiding); if
///   head.back() == tail.front() that middle is the one node. The candidate route is head,
///   middle, tail with the two joints counted once. The route is the candidate of lowest
///   delay; ties go to the earlier head, then the earlier tail (the first one found). No
///   candidate joined (or no candidates at all): no route.
///
/// The held route is "usable" when it is not empty, starts at `source`, ends at `target` and
/// is up in the planning graph. (A held route always does start and end there when every plan
/// of one object is given the same source and target, which is the intended use.)
///
/// Preconditions: source and target are below graph.node_count() and differ; every node of
/// every candidate is below graph.node_count() (one that is not makes that head or tail "not
/// up", and it is skipped).
class RoutePlanner {
 public:
  explicit RoutePlanner(Strategy strategy) : strategy_(strategy) {}

  [[nodiscard]] const Strategy& strategy() const noexcept { return strategy_; }

  /// Re-plans with `graph` as the planning graph (see the class comment). `candidates` is
  /// read only by longest-lived.
  PlanResult plan(const network::Graph& graph, std::size_t source, std::size_t target,
                  const AccessCandidates& candidates = {});

  /// plan, with the shortest path from source to target in `graph` given by the caller, so
  /// that one search serves every direction and strategy that plans from the same source.
  /// `shortest_path` must be network::shortest_paths(graph, source).path_to(target) (empty
  /// when the target is unreachable); the result and the held route are then exactly those of
  /// plan(graph, source, target, candidates). Longest-lived does not read it.
  PlanResult plan_with_shortest(const network::Graph& graph, std::size_t source, std::size_t target,
                                const Route& shortest_path,
                                const AccessCandidates& candidates = {});

  /// Replaces the held route with `route` without planning (Phase 4 spec 4.5: local repair
  /// hands a direction its repaired route). Empty means no route. Nothing is checked: the
  /// route need not be up in any graph, nor start or end anywhere in particular. From then on
  /// it is the held route like any other: route(), held_route_is_up and held_delay_s report
  /// on it, and the next plan treats it as the route held before that plan (so keep-until-
  /// broken keeps it if it is usable, and the outcome is kept or replaced against it).
  void hold(Route route) noexcept { route_ = std::move(route); }

  /// The held route; empty when there is none.
  [[nodiscard]] const Route& route() const noexcept { return route_; }

  [[nodiscard]] bool has_route() const noexcept { return !route_.empty(); }

  /// Whether the held route is up in a step's graph; false when there is no route.
  [[nodiscard]] bool held_route_is_up(const network::Graph& step_graph) const noexcept;

  /// The held route's delay in a step's graph, seconds; nullopt when there is no route or it
  /// is not up.
  [[nodiscard]] std::optional<double> held_delay_s(const network::Graph& step_graph) const noexcept;

 private:
  PlanResult plan_from(const network::Graph& graph, std::size_t source, std::size_t target,
                       const Route* shortest_path, const AccessCandidates& candidates);

  Strategy strategy_;
  Route route_;
};

}  // namespace satsim::routing
