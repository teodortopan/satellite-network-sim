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
  [[nodiscard]] std::optional<std::string> step(std::int64_t k, const Lookahead& ahead,
                                                std::vector<RouteRow>& rows);

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

  // At a slot start: every dish's serving satellite.
  [[nodiscard]] std::optional<std::string> choose_serving(const Lookahead& ahead,
                                                          const StepGround& now);
  [[nodiscard]] std::optional<std::size_t> pick_by_rule(const Lookahead& ahead,
                                                        const StepGround& now, std::size_t city);
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
  // The first missing edge of a broken route, as a cause.
  [[nodiscard]] std::optional<std::pair<DropCause, Responsible>> broken_edge_cause(
      const StepGround& now, const routing::Route& route) const;
  // The shortest path from `source` to `target` in `graph`, sharing one search per source.
  [[nodiscard]] routing::Route shortest(const network::Graph& graph, std::size_t source,
                                        std::size_t target,
                                        std::map<std::size_t, network::ShortestPaths>& cache) const;
  [[nodiscard]] bool route_hops_exist(const StepGround& now, const routing::Route& route,
                                      std::size_t d, std::size_t s) const;
  void plan_all(std::int64_t k, const Lookahead& ahead, const StepGround& now,
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
  network::Graph planning_graph_;
  network::Graph open_graph_;
  /// Per site: time left in view of its visible satellites at the current step (cache).
  std::map<std::size_t, std::vector<double>> times_left_;
  /// Shortest-path trees in this step's graph, by source node (cache).
  std::map<std::size_t, network::ShortestPaths> step_trees_;
  std::optional<std::string> plan_error_;
};

}  // namespace satsim::routing_app
