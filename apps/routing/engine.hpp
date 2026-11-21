#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// The planner loop of Phase 3 spec 4.3 to 4.7: one held route per direction and strategy,
// re-planned on a fixed interval, checked at every step, with serving slots (realistic), drop
// causes, drop events, the measures and the route checks of spec 6.1. Pure: the steps come from
// a Lookahead (the program's propagated snapshot, or a test's tables).
//
// Phase 4 (spec 4.5, 4.6) gives it two: the truth, what works, and a view, what the planner
// believes. The serving choice and every plan read the view; the step graph, the ideal, whether
// the held route is up, its delay, its drop cause, the checks and the measures read the truth.
// With one object for both it is the Phase 3 engine exactly.

namespace satsim::routing_app {

/// What the engine may know at the current step about it and the steps after it.
class Lookahead {
 public:
  Lookahead() = default;
  Lookahead(const Lookahead&) = default;
  Lookahead& operator=(const Lookahead&) = default;
  Lookahead(Lookahead&&) = default;
  Lookahead& operator=(Lookahead&&) = default;
  virtual ~Lookahead() = default;

  /// Steps from now (offset 0) whose link states and ground links are known; at least 1.
  [[nodiscard]] virtual std::size_t known_steps() const = 0;
  /// Precondition: offset < known_steps().
  [[nodiscard]] virtual StepGround step(std::size_t offset) const = 0;
  /// The time left in view (spec 4.4, topology::time_left_in_view_s) from site `site` (cities
  /// first, then gateways) of each satellite of `visible`, in that order, with `horizon_s`.
  [[nodiscard]] virtual std::vector<double> times_left_in_view_s(
      std::size_t site, std::span<const topology::VisibleSatellite> visible,
      double horizon_s) const = 0;
  /// Whether a satellite moves north now: its Earth-fixed z is larger at the next step than at
  /// this one (false when either position is missing).
  [[nodiscard]] virtual bool moving_north(std::size_t satellite) const = 0;
};

/// One direction of a city pair, as the engine sees it.
struct EngineDirection {
  std::size_t from = 0;  ///< City index.
  std::size_t to = 0;
  double lower_bound_s = 0.0;  ///< Great-circle distance / c.
  /// Realistic: the sender has a home PoP (otherwise every step is no_service).
  bool has_home_pop = true;
};

/// What the engine runs on.
struct EngineNetwork {
  EndpointModel model = EndpointModel::kRealistic;
  std::size_t satellite_count = 0;
  std::size_t city_count = 0;
  std::size_t gateway_count = 0;
  /// The laser links (outlives the engine).
  const topology::LinkPlan* plan = nullptr;
  std::vector<EngineDirection> directions;
  /// Realistic: [d * G + g], the fibre delay from gateway g to direction d's end node (see
  /// GraphBuilder), NaN for a sender without a home PoP.
  std::vector<double> end_delays_s;
};

struct EngineSettings {
  std::vector<routing::Strategy> strategies;
  std::int64_t step_ns = 1'000'000'000;
  /// Multiples of step_ns, > 0.
  std::int64_t replan_ns = 15'000'000'000;
  std::int64_t slot_ns = 15'000'000'000;
  Planner planner = Planner::kStart;
  ServingRule serving = ServingRule::kHighestElevation;
  bool backup = false;
  double hop_delay_s = 0.0;
};

/// The longest a satellite's time left in view is predicted, s (spec 4.4).
inline constexpr double kViewHorizonS = 900.0;

/// The look-ahead an engine with these settings needs: positions this many steps from now (at
/// least 1) for the time left in view and the direction of travel, and links and ground links
/// this many steps from now (at least 1, the whole-interval planner's interval).
[[nodiscard]] std::size_t position_steps_needed(const EngineSettings& settings,
                                                EndpointModel model);
[[nodiscard]] std::size_t full_steps_needed(const EngineSettings& settings);

/// A site that is responsible for a drop.
struct SiteRef {
  bool gateway = false;  ///< Otherwise a city.
  std::size_t index = 0;

  friend bool operator==(const SiteRef&, const SiteRef&) = default;
};

/// What is responsible for a drop (spec 4.6); unset fields do not apply.
struct Responsible {
  std::optional<std::size_t> satellite;
  /// Index into LinkPlan::links, with the reason it is down.
  std::optional<std::size_t> link;
  std::optional<topology::LinkDownReason> link_reason;
  std::optional<SiteRef> site;

  friend bool operator==(const Responsible&, const Responsible&) = default;
};

/// What a lane is at an instant, once planned (Engine::evaluate): up with a delay, or a drop
/// with its cause (none only for a lane holding no route with no plan failure stored).
struct LaneState {
  bool ok = false;
  /// The held route's delay in the given graph; set exactly when ok.
  std::optional<double> delay_s = std::nullopt;
  std::optional<DropCause> cause = std::nullopt;
  Responsible responsible{};
};

/// One direction under one strategy at one step.
struct RouteRow {
  std::size_t direction = 0;
  std::size_t strategy = 0;
  /// The held route is up in this step's graph.
  bool ok = false;
  /// Set exactly for a drop (a dropped step without one fails a check).
  std::optional<DropCause> cause;
  Responsible responsible;
  /// The held route's delay in this step's graph (ok only).
  double delay_s = 0.0;
  /// The lowest delay at this step from the sending city to the target, with the dish free to
  /// use any satellite it sees (spec 2, 4.7); nullopt when there is none.
  std::optional<double> ideal_s;
  /// The held route (node numbers of GraphLayout); may be non-empty on a drop (it broke).
  routing::Route route;
  /// Realistic: the dish's serving satellite in this slot.
  std::optional<std::size_t> serving;
  /// Set at a re-plan instant: what the plan did.
  std::optional<routing::PlanOutcome> plan;
  /// At a re-plan instant with the backup on and a route: the backup path's delay in the
  /// planning graph (nullopt when there is none).
  std::optional<double> backup_s;
};

/// Consecutive dropped steps of one direction and strategy with one cause (spec 4.6).
struct DropEvent {
  std::int64_t start_step = 0;
  std::int64_t steps = 0;
  std::size_t direction = 0;
  std::size_t strategy = 0;
  DropCause cause = DropCause::kNoPath;
  /// What was responsible at the event's first step.
  Responsible responsible;
};

/// The measures of spec 4.7 for one direction and strategy.
struct Measures {
  std::size_t steps = 0;
  std::size_t ok_steps = 0;
  std::size_t bad_changes = 0;
  std::size_t increase_changes = 0;
  std::size_t breaks = 0;
  std::size_t recoveries = 0;
  /// New route's delay at its first step minus the old route's at its last, ms, per change.
  std::vector<double> change_sizes_ms;
  /// How long each route was used without a change or a drop, s (routes cut by the run's
  /// start or end included).
  std::vector<double> lifetimes_s;
  std::array<std::size_t, kAllDropCauses.size()> drop_steps_by_cause{};
  std::size_t drop_events = 0;
  /// Of the ok steps: the delay, and the delay minus the ideal, ms.
  std::vector<double> delay_ms;
  std::vector<double> extra_ms;
  std::size_t plans = 0;
  std::size_t kept = 0;
  std::size_t replaced = 0;
  std::size_t no_route = 0;
  /// At re-plan instants with a route and the backup on: the backup's delay, its delay minus
  /// the held route's (both in the planning graph), ms; and how often there was none.
  std::vector<double> backup_ms;
  std::vector<double> backup_extra_ms;
  std::size_t backup_missing = 0;
};

/// Violations of the route checks of spec 6.1 (the physical checks are the program's). All 0
/// for a correct run.
struct RouteChecks {
  /// An ok route with a hop that the step's links and ground links do not give (rechecked
  /// from them, not from the graph).
  std::size_t ok_route_not_in_graph = 0;
  /// An ok delay below the ideal of the same step (or an ok step without an ideal).
  std::size_t below_ideal = 0;
  /// An ok delay below great circle / c.
  std::size_t below_lower_bound = 0;
  std::size_t drop_without_cause = 0;
  /// A held route that does not start at the direction's source, end at its target, or has a
  /// node twice.
  std::size_t bad_route_shape = 0;

  [[nodiscard]] std::size_t total() const noexcept {
    return ok_route_not_in_graph + below_ideal + below_lower_bound + drop_without_cause +
           bad_route_shape;
  }
};

/// Called on every row before the checks: a test seam (never set by the program).
using BeforeChecks = std::function<void(RouteRow&)>;

class Engine {
 public:
  /// Preconditions: settings.strategies not empty; replan_ns and slot_ns positive multiples of
  /// step_ns; network.plan set; directions name cities below city_count.
  Engine(EngineNetwork network, EngineSettings settings, BeforeChecks before_checks = {});

  /// Step number k (from 0, called in order). Writes one row per direction and strategy into
  /// `rows`, direction-major. Returns an error only if a graph cannot be built.
  /// step(k, ahead, rows) is step(k, ahead, ahead, rows): the Phase 3 engine.
  [[nodiscard]] std::optional<std::string> step(std::int64_t k, const Lookahead& ahead,
                                                std::vector<RouteRow>& rows);

  /// step, with the truth and a view apart (Phase 4 spec 4.5). From `view`: at a slot start
  /// every dish's serving satellite (the rule's choice among what the view's dish sees, and
  /// with-route's trial plan), and at a re-plan instant everything of the plan (planning
  /// graph, longest-lived's candidates and times left in view, the cause stored when a plan
  /// finds no route, the backup). With the start planner the planning graph is the view's
  /// graph of this step; whole-interval takes the view's look-ahead. From `truth`: the step
  /// graph, the ideal, whether the held route is up and its delay, the drop cause (evaluate),
  /// the route checks and the measures. Passing the same object for both is exactly
  /// step(k, ahead, rows) (the step graph and its shortest-path trees then serve the plan).
  [[nodiscard]] std::optional<std::string> step(std::int64_t k, const Lookahead& truth,
                                                const Lookahead& view, std::vector<RouteRow>& rows);

  /// A plan of every lane now, between two regular ones (spec 4.5 --replan-on-report): what
  /// step does at a re-plan instant, with the serving satellites as they are (no slot start),
  /// on the view (and, with the same object, as step does). `k` is the step whose geometry
  /// truth and view hold (the last step at or before now). Rebuilds the step graph from
  /// `truth` with the dishes' current serving satellites. Writes one row per lane (direction-
  /// major) holding only direction, strategy, serving, plan, backup_s and the route now held;
  /// nothing is checked or recorded, except that the plan counts in the measures (plans, kept,
  /// replaced, none, backup) as every plan does. Returns an error only if a graph cannot be
  /// built.
  [[nodiscard]] std::optional<std::string> replan(std::int64_t k, const Lookahead& truth,
                                                  const Lookahead& view,
                                                  std::vector<RouteRow>& rows);

  /// The graph the last plan was made on: the view's step graph (the step graph itself when
  /// truth and view were one object) or the whole-interval graph. Valid until the next call
  /// of step or replan.
  [[nodiscard]] const network::Graph& planning_graph() const noexcept;

  /// The route lane (d, s) holds; empty for none.
  [[nodiscard]] const routing::Route& lane_route(std::size_t d, std::size_t s) const;

  /// Replaces lane (d, s)'s held route without planning (local repair, spec 4.5;
  /// RoutePlanner::hold). A non-empty route also clears the cause stored by a plan that found
  /// none. From the next step (or evaluate) on it is the held route like any other.
  void hold_route(std::size_t d, std::size_t s, routing::Route route);

  /// What step decides for lane (d, s) after planning, against `truth_now` and `truth_graph`
  /// (built from truth_now with builder()) instead of the engine's own step graph; nothing is
  /// recorded. Up in the graph: ok with its delay. Otherwise the cause, the first that applies:
  ///  1. Failures (Phase 4 spec 4.6). Realistic, a direction with a home PoP: the dish's
  ///     serving satellite marked down is failed_satellite (that satellite). Then the held
  ///     route in order, node by node and each laser edge after its first node: a satellite
  ///     marked down, failed_satellite (the satellite); a gateway marked down, failed_gateway
  ///     (the gateway site); a laser edge whose link is down with LinkDownReason::kFailed and
  ///     neither end marked down, failed_link (the link, reason kFailed).
  ///  2. Phase 3 spec 4.6 unchanged: no_service, no_coverage, serving_left_view; for a held
  ///     route its first missing edge (ground_link_lost, laser_link_down); with no route the
  ///     cause the last plan stored.
  [[nodiscard]] LaneState evaluate(std::size_t d, std::size_t s, const StepGround& truth_now,
                                   const network::Graph& truth_graph) const;

  /// Dish `dish`'s serving satellite (realistic; dish_of numbers them), nullopt for none.
  [[nodiscard]] std::optional<std::size_t> serving_of(std::size_t dish) const;
  /// Gives dish `dish` another serving satellite (or none) until the next slot start (local
  /// repair, spec 4.5). The next step's graph and rows use it.
  void set_serving(std::size_t dish, std::optional<std::size_t> satellite);

  /// The builder of every graph the engine makes (for graphs built from other grounds).
  [[nodiscard]] const GraphBuilder& builder() const noexcept { return builder_; }

  /// Ends the run: closes the open drop events and route lifetimes.
  void finish();

  [[nodiscard]] const GraphLayout& layout() const noexcept { return builder_.layout(); }
  [[nodiscard]] const EngineNetwork& network() const noexcept { return network_; }
  [[nodiscard]] const EngineSettings& settings() const noexcept { return settings_; }
  /// [direction * strategies + strategy].
  [[nodiscard]] const std::vector<Measures>& measures() const noexcept { return measures_; }
  /// In the order they ended; finish() closes the rest.
  [[nodiscard]] const std::vector<DropEvent>& drop_events() const noexcept { return events_; }
  [[nodiscard]] const RouteChecks& checks() const noexcept { return checks_; }
  /// The step graph of the last step (for tests).
  [[nodiscard]] const network::Graph& step_graph() const noexcept { return step_graph_; }

  /// The node a direction's route starts at under a strategy, and the node it ends at.
  [[nodiscard]] std::size_t source_node(std::size_t direction, std::size_t strategy) const;
  [[nodiscard]] std::size_t target_node(std::size_t direction) const;
  /// The serving dish a direction uses under a strategy (realistic).
  [[nodiscard]] std::size_t dish_of(std::size_t direction, std::size_t strategy) const;

 private:
  /// One direction and strategy.
  struct Lane {
    routing::RoutePlanner planner;
    /// Why the last plan found no route, and what was responsible.
    std::optional<DropCause> plan_failure;
    Responsible plan_failure_responsible;
    // The previous step, for changes and lifetimes.
    bool has_previous = false;
    routing::Route previous_route;
    bool previous_ok = false;
    double previous_delay_s = 0.0;
    std::int64_t lifetime_steps = 0;
    std::optional<DropEvent> open_event;
  };

  /// A dish's serving satellite in the current slot.
  struct Dish {
    std::size_t city = 0;
    std::optional<std::size_t> serving;
  };

  [[nodiscard]] bool realistic() const noexcept {
    return network_.model == EndpointModel::kRealistic;
  }
  [[nodiscard]] std::size_t lane_index(std::size_t d, std::size_t s) const noexcept {
    return d * settings_.strategies.size() + s;
  }

  /// Which graph the last plan was made on.
  enum class PlanGraph : std::uint8_t { kStep, kView, kInterval };

  // At a slot start: every dish's serving satellite, from the view.
  [[nodiscard]] std::optional<std::string> choose_serving(const Lookahead& view,
                                                          const StepGround& seen);
  [[nodiscard]] std::optional<std::size_t> pick_by_rule(const Lookahead& view,
                                                        const StepGround& seen, std::size_t city);
  // The longest-lived access candidates of a lane in `graph`.
  [[nodiscard]] routing::AccessCandidates candidates(const network::Graph& graph,
                                                     const Lookahead& ahead, const StepGround& now,
                                                     std::size_t d, std::size_t s,
                                                     const routing::Route& shortest,
                                                     bool open_head);
  // The time left in view of every satellite `site` sees now (cached per step).
  [[nodiscard]] const std::vector<double>& times_left(const Lookahead& ahead, const StepGround& now,
                                                      std::size_t site);
  // A step-level cause (no_service, no_coverage, serving_left_view), or nullopt.
  [[nodiscard]] std::optional<std::pair<DropCause, Responsible>> cause_now(const StepGround& now,
                                                                           std::size_t d,
                                                                           std::size_t s) const;
  // The first failed element a lane meets (evaluate's rule 1), or nullopt.
  [[nodiscard]] std::optional<std::pair<DropCause, Responsible>> failure_cause(
      const StepGround& now, std::size_t d, std::size_t s, const routing::Route& route) const;
  // The first edge of a broken route missing from `graph`, as a cause.
  [[nodiscard]] std::optional<std::pair<DropCause, Responsible>> broken_edge_cause(
      const StepGround& now, const network::Graph& graph, const routing::Route& route) const;
  // The serving satellite of every dish, in dish order.
  [[nodiscard]] std::vector<std::optional<std::size_t>> serving_list() const;
  // The shortest path from `source` to `target` in `graph`, sharing one search per source.
  [[nodiscard]] routing::Route shortest(const network::Graph& graph, std::size_t source,
                                        std::size_t target,
                                        std::map<std::size_t, network::ShortestPaths>& cache) const;
  [[nodiscard]] bool route_hops_exist(const StepGround& now, const routing::Route& route,
                                      std::size_t d, std::size_t s) const;
  // The plan of every lane on the view; `same`: the view is the truth's object (the step
  // graph and its trees are then the planning graph's).
  void plan_all(std::int64_t k, const Lookahead& view, const StepGround& seen, bool same,
                std::vector<RouteRow>& rows);
  void record(std::int64_t k, const StepGround& now, RouteRow& row);
  void close_event(Lane& lane);

  EngineNetwork network_;
  EngineSettings settings_;
  BeforeChecks before_checks_;
  GraphBuilder builder_;
  std::int64_t replan_steps_ = 1;
  std::int64_t slot_steps_ = 1;
  std::vector<Lane> lanes_;
  std::vector<Dish> dishes_;
  /// (min(a, b), max(a, b)) -> index into plan->links.
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> link_index_;
  std::vector<Measures> measures_;
  std::vector<DropEvent> events_;
  RouteChecks checks_;
  network::Graph step_graph_;
  /// The whole-interval planning graph.
  network::Graph planning_graph_;
  /// The start planner's graph of the view's step, when the view is not the truth's object.
  network::Graph view_graph_;
  PlanGraph plan_graph_ = PlanGraph::kStep;
  network::Graph open_graph_;
  /// Per site: time left in view of its visible satellites at the current step (cache).
  std::map<std::size_t, std::vector<double>> times_left_;
  /// Shortest-path trees in this step's graph, by source node (cache).
  std::map<std::size_t, network::ShortestPaths> step_trees_;
  std::optional<std::string> plan_error_;
};

}  // namespace satsim::routing_app
