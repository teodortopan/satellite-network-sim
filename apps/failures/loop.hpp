#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "failures/flooding.hpp"
#include "failures/geometry.hpp"
#include "failures/settings.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "routing/engine.hpp"
#include "routing/settings.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

// The failures program's loop (Phase 4 spec 4.3, 4.5 to 4.7, 6.1; docs/phase-4-plan.md, design
// choices 3 and 4): one routing_app::Engine per lane kind (the baseline, the reference and each
// reaction mode), stepped together through the run, with the events between two steps worked
// out in continuous time on the step's frozen geometry. Pure: the steps come from a StepWorld
// (the program's propagated snapshot, or a test's tables) and the failures from a Timeline.
//
// Lanes. Every direction and strategy has one lane per engine:
//  - baseline: the network with no failure at all (the empty view, as truth and view);
//  - reference: a planner that knows the true network at every plan (truth as truth and view);
//  - planner: the truth, and the planner's view (what the planner knows at the instant, on each
//    step's geometry, view_at(planner_knowledge));
//  - local-repair: as planner, and the node before a dead element switches to a backup.
// The reference plans only at the regular re-plan instants, also with --replan-on-report.
//
// Instants. At step k (instant k * step_s) every engine steps (Engine::step). Then, in increasing
// order, each instant t in (k * step_s, (k + 1) * step_s) at which something can change: a
// change of the truth (Timeline::changes), the planner learning a fact (with --replan-on-report,
// which re-plans the planner and local-repair engines there), and a local repair. At each one the
// truth at t (overlay_truth) is built with each engine's serving satellites and every lane of
// every engine but the baseline is evaluated again (Engine::evaluate). The baseline changes only
// at steps. At one instant (a step's or one inside it) the local repairs due come before a plan
// on a report (spec 4.5 Details), which then sees the repaired dishes and routes; the repairs
// that plan makes due (a route handed through an element its node knows is dead) follow it.
//
// Local repair (spec 4.5). Right after every plan of the local-repair engine its planning graph
// is copied; a backup is the shortest path in that copy (network::shortest_paths_avoiding) from
// a route node to the route's end, never entering the node after it nor any node before it,
// worked out when it is needed (the same path as one prepared at the plan). A lane in
// black_hole is repaired by the node just before the first dead element along its route (the
// near end of a dead link), at max(t_pass, its knowing instant), where t_pass is the start of
// the lane's black-hole interval (the instant its route first passes the dead element) and the
// knowing instant is the node's detection instant of that element if it is one of the fact's
// detectors (a satellite before a dead satellite: the fact "link down" of the link between
// them; before a dead gateway: "gateway down"; a best-case dish before a dead satellite:
// "satellite down"; the near end of a dead link: "link down"; an old fact: instant 0), else
// t_pass + detection_s. The route becomes the old route up to that node, then its backup
// (Engine::hold_route); no backup: nothing changes. One repair per lane between two plans.
// Realistic, the dead element is the dish's serving satellite: its pass instant is the later of
// the satellite's failure instant and the instant the dish got it (whatever route its lanes
// hold), and at the earliest repair instant of the city's lanes the dish picks again
// (Engine::pick_serving) among the satellites the city sees in the truth, the dead one left
// out, and every lane of the city (of a direction with a home PoP), holding a route or not (a
// plan that found none because of the dead serving satellite cleared it), continues from the
// new satellite on the shortest path in the copied planning graph that avoids the dead one (no
// new satellite, or no such path: nothing changes). A repaired route is kept at a plan whose
// planning graph still has the dead hop (the edge from the repairing node to the old route's next
// node; for a dish, from the dish to the dead satellite): after the engine planned, the repaired
// route is held again and the plan written `kept`. At the first plan without that hop the lane is
// planned as usual and may repair again. A dish's repaired satellite is given back at a slot
// start that picks the dead one again.
//
// Flooding (spec 4.5, 4.6; the program's options: lowest-delay only). One more engine, whose
// lanes of the lowest-delay strategy are the flooding lanes (its other lanes stay unused); it
// never plans: its routing_app::Engine only names causes (evaluate), holds the dishes' serving
// satellites and numbers the nodes. Every node answers from its own table (flooding.hpp):
//  - Plans. At every regular re-plan instant the superset planning graph is built: the
//    GraphBuilder graph of the plan's window (the step for the start planner; for the
//    whole-interval planner every step of the interval the world knows, the serving dish edges
//    tested over the rest of the slot), on overlay_view of all_in_force_view, with the
//    flooding dishes' serving satellites. It is the baseline's planning graph when nothing has
//    failed.
//  - The actor of a graph node: a satellite or gateway is itself; a city's dish nodes are the
//    city's dish; an end node is a target. Its table at t is FloodTables::tree of the facts it
//    uses at t (FloodKnowledge::facts_at).
//  - A realistic serving dish sends to its serving satellite S, if S is in the window's lists
//    of its city at each of the steps the planning graph tests a serving dish edge at (the
//    edge the graph would have with S as serving satellite), S is not down in its view and S's
//    table reaches the target; else it has no next hop. A dish knows only what it found out
//    itself (and the old facts, from instant 0). Its serving satellite: at every slot start the
//    serving rule (Engine::pick_serving) among what its city sees by geometry (the step's
//    visible list, failed satellites in it), leaving out only the satellites it knows are dead
//    and uses that; so it may pick a dead satellite it cannot know about, a black hole on it.
//    When S, serving since instant u, is down at u or goes down at t_f >= u, the dish knows at
//    u + detection_s or t_f + detection_s (if S is still down then) and picks again by the rule
//    at that instant plus compute_s, from which it uses that S is dead: among what its city
//    sees by geometry, every satellite it uses as dead left out; with nothing else to pick it
//    keeps S. It forgets nothing during a run: a satellite it knows as dead that returns is
//    never tried again (it does not learn of the return; a consequence of the rule).
//  - At an instant the traffic follows routing::walk from the lane's source node to its
//    target, then is judged against the truth graph (the truth at the instant with the
//    flooding dishes' serving satellites): every hop up and delivered: delivered, delay
//    network::path_delay_s. Else at the first hop that is not up: a down satellite or gateway
//    after it, or a failed laser link with both ends working: black_hole with that element
//    and its failed_* cause, the route up to the hop's first node; any other missing hop:
//    no_route with the walk as the route and the cause Engine::evaluate gives the walk held as
//    a route (Phase 3's rule for a broken route), or, if that names a failure further along,
//    the cause it gives the walk up to the missing hop. All hops up and a dead end: at the
//    source, the cause of Engine::evaluate with no route held (realistic serving satellite
//    down: a black hole on it; no_service, no_coverage, serving_left_view), else no_path (best
//    case) or no_gateway (realistic), with no route; at a later node a black_hole with no
//    cause nor element, the route up to it. All hops up and a loop: loop, the walk as route.
//  - Rows: at a re-plan instant the plan is none without a walk from the source, kept when
//    the walk is the previous step's, else replaced; the ideal is the reference's.
//  - The lanes are evaluated at every step instant, every instant of the other lanes, and
//    every instant inside a step at which a node of the lane's current walk (its source
//    included) starts using a fact or a flooding dish picks again.
//
// Intervals (spec 4.6, 4.10). Per lane, the open interval (state, route, cause, element) is
// closed and a new one opened when one of them changes; an interval that would last zero
// seconds is not kept. The run ends at its last step's instant, (step_count - 1) * step_s,
// where the last interval closes; nothing is worked out after it.

namespace satsim::failures_app {

/// What a direction experiences at an instant (spec 4.6).
enum class DirectionState : std::uint8_t {
  kDelivered,  ///< Its route works: the engine says ok.
  kBlackHole,  ///< Its route reaches a failed element (a failed_* drop cause).
  kLoop,       ///< It passes a node twice (flooding only).
  kNoRoute,    ///< Any other drop.
};

/// "delivered", "black_hole", "loop", "no_route".
[[nodiscard]] std::string_view name_of(DirectionState state) noexcept;

/// The lanes of a direction and strategy, in output order.
enum class LaneKind : std::uint8_t { kBaseline, kReference, kPlanner, kLocalRepair, kFlooding };

/// "baseline", "reference", "planner", "local-repair", "flooding".
[[nodiscard]] std::string_view name_of(LaneKind kind) noexcept;

/// The geometry of the current step and the steps after it, as the loop reads it.
class StepWorld {
 public:
  StepWorld() = default;
  virtual ~StepWorld() = default;
  StepWorld(const StepWorld&) = delete;
  StepWorld& operator=(const StepWorld&) = delete;
  StepWorld(StepWorld&&) = delete;
  StepWorld& operator=(StepWorld&&) = delete;

  /// Steps from now (offset 0) whose geometry is known; at least 1.
  [[nodiscard]] virtual std::size_t known_steps() const = 0;
  /// The step's UTC time and visible lists (positions are not read). offset < known_steps().
  [[nodiscard]] virtual const StepGeometry& geometry(std::size_t offset) const = 0;
  /// The geometric state of every link of Timeline::links at that step.
  [[nodiscard]] virtual const topology::StepLinks& geometric(std::size_t offset) const = 0;
  /// As routing_app::Lookahead.
  [[nodiscard]] virtual std::vector<double> times_left_in_view_s(
      std::size_t site, std::span<const topology::VisibleSatellite> visible,
      double horizon_s) const = 0;
  [[nodiscard]] virtual bool moving_north(std::size_t satellite) const = 0;
};

/// A lane at an instant.
struct Status {
  DirectionState state = DirectionState::kNoRoute;
  /// Set for every state but delivered (none only for a lane that never planned).
  std::optional<routing_app::DropCause> cause = std::nullopt;
  /// The failed element of a black hole (the engine's responsible element).
  std::optional<Subject> element = std::nullopt;
  /// The held route (graph nodes of the engine's layout); may be set on a drop.
  routing::Route route;
  /// The delivered delay.
  std::optional<double> delay_s = std::nullopt;

  /// Whether two statuses belong to one interval: same state, route, cause and element.
  [[nodiscard]] bool same_interval(const Status& other) const {
    return state == other.state && route == other.route && cause == other.cause &&
           element == other.element;
  }
};

/// One interval of one lane (spec 4.10 events CSV): the status at its start.
struct Interval {
  std::size_t engine = 0;  ///< Index into the loop's engines (LaneKind order of kinds()).
  std::size_t direction = 0;
  std::size_t strategy = 0;
  double start_s = 0.0;
  double end_s = 0.0;
  Status status;
  /// Realistic: the dish's serving satellite at the start.
  std::optional<std::size_t> serving = std::nullopt;
};

/// Violations of the new checks of spec 6.1 (the Phase 3 route checks are each engine's, the
/// physical and force checks the program's). All 0 for a correct run.
struct FailureChecks {
  /// A delivered lane (at a step instant or an interval start) whose route has a hop that the
  /// truth at that instant does not give: a laser link that is not up, a satellite a site does
  /// not see (failed satellites are not seen), a dish edge to another satellite than the serving
  /// one, a hop of a kind the graph has not (rechecked from the truth, not from the graph).
  std::size_t delivered_not_up = 0;
  /// A black-hole lane whose element is not down in the truth at that instant or is not on its
  /// route (nor, realistic, the dish's serving satellite); in flooding, one whose element is
  /// not down in the truth or is not where the walk goes after the route, or, without an
  /// element, whose route's last node has a next hop.
  std::size_t black_hole_not_failed = 0;
  /// A plan of the planner or local-repair engine made on a view that differs from the one the
  /// facts give with the planner's knowing instants (Fact::planner_known_s) at the plan instant:
  /// the planner used a fact too early (or one it had).
  std::size_t fact_used_early = 0;
  /// A local repair before its element's failure instant plus the detection time, or before
  /// its route passed the element.
  std::size_t repair_too_early = 0;
  /// Once settled (the first regular plan instant, realistic also a slot start, at or after the
  /// last change of the truth and the last instant the planner learns a fact, all of which it
  /// learns), a lowest-delay lane of a reaction mode at a step whose state or route is not the
  /// reference's.
  std::size_t settled_differs = 0;
  /// With re-pointing off, a delivered delay below the baseline's ideal at that step (or a
  /// delivered lane at a step where the baseline has no ideal).
  std::size_t below_baseline_ideal = 0;
  /// Flooding: a loop interval whose route does not end at a node it already passed.
  std::size_t loop_not_repeated = 0;
  /// Flooding: a table used at an instant before the node's learning instant of one of its
  /// facts plus compute_s (FloodKnowledge::early).
  std::size_t table_fact_early = 0;
  /// Flooding: a flood that missed a node its messages graph joins to a detector
  /// (Floods::unreached; the program adds it).
  std::size_t flood_unreached = 0;
  /// Flooding, once settled (the first regular plan instant, realistic also a slot start, at
  /// or after the last change of the truth and the last instant any node starts using a fact,
  /// every fact that is not old having a flood): a step whose state or route is not the
  /// reference's (counted in settled_differs), or a delivered route from one of whose nodes
  /// the tables do not give the rest of it (counted here).
  std::size_t settled_tables_differ = 0;

  [[nodiscard]] std::size_t total() const noexcept {
    return delivered_not_up + black_hole_not_failed + fact_used_early + repair_too_early +
           settled_differs + below_baseline_ideal + loop_not_repeated + table_fact_early +
           flood_unreached + settled_tables_differ;
  }
};

struct LoopSettings {
  routing_app::EngineSettings engine{};
  /// The reaction modes, each once. With flooding, engine.strategies must hold lowest-delay
  /// and `floods` must be set.
  std::vector<Reaction> reactions;
  bool replan_on_report = false;
  ReactionTimes times{};
  /// Steps of the run (>= 1).
  std::int64_t step_count = 1;
  /// Flooding: every fact's flood (flood_facts on the same timeline); must outlive the loop.
  const Floods* floods = nullptr;
};

/// Test seams; never set by the program.
struct LoopHooks {
  /// Changes the planner's knowledge (Timeline facts' planner_known_s) before the run.
  std::function<void(Knowledge&)> planner_knowledge;
  /// Called on every status (engine, direction, strategy) after it is worked out at an instant,
  /// before the checks and intervals.
  std::function<void(std::size_t, std::size_t, std::size_t, Status&)> before_checks;
  /// Changes a local repair's instant once it is worked out.
  std::function<double(double)> repair_instant;
  /// Given to every engine (routing_app::BeforeChecks).
  routing_app::BeforeChecks engine_before_checks;
  /// Changes a flooding node's use instant of a fact (FloodKnowledge::UseHook).
  FloodKnowledge::UseHook flood_use;
};

/// One row of the routes CSV: an engine's RouteRow at a step, with the lane's status at the
/// step's instant (after the repairs made at that instant) written into its ok, cause,
/// responsible, delay, route and serving.
struct LaneRow {
  std::size_t engine = 0;
  routing_app::RouteRow row;
  Status status;
};

class FailureLoop {
 public:
  /// Everything the loop keeps between steps (loop.cpp).
  struct State;

  /// `network` without its plan (the loop's own, of timeline.links, is used). `timeline` must
  /// outlive the loop.
  FailureLoop(const Timeline& timeline, routing_app::EngineNetwork network, LoopSettings settings,
              LoopHooks hooks = {});
  ~FailureLoop();
  FailureLoop(const FailureLoop&) = delete;
  FailureLoop& operator=(const FailureLoop&) = delete;
  FailureLoop(FailureLoop&&) = delete;
  FailureLoop& operator=(FailureLoop&&) = delete;

  /// Step k (from 0, in order), then the instants inside it. Writes the rows of the step's
  /// instant, ordered by direction, strategy, then engine (the flooding engine's only under
  /// its strategy). Returns an error only if a graph cannot be built.
  [[nodiscard]] std::optional<std::string> step(std::int64_t k, const StepWorld& world,
                                                std::vector<LaneRow>& rows);

  /// Ends the run: closes every interval at end_s() and sorts them by direction, strategy,
  /// engine, then start; finishes every engine.
  void finish();

  /// The engines, in output order: baseline, reference, then the reaction modes as listed.
  [[nodiscard]] std::size_t engine_count() const noexcept;
  [[nodiscard]] LaneKind kind(std::size_t engine) const;
  [[nodiscard]] const routing_app::Engine& engine(std::size_t engine) const;

  /// After finish(): every interval of every lane.
  [[nodiscard]] const std::vector<Interval>& intervals() const noexcept { return intervals_; }
  [[nodiscard]] const FailureChecks& checks() const noexcept { return checks_; }
  /// The truth at the last step's instant.
  [[nodiscard]] const OverlaidStep& truth() const;
  /// Per engine, direction and strategy ([(e * D + d) * S + s]), per step done: the delivered
  /// delay at the step's instant, nullopt when not delivered.
  [[nodiscard]] const std::vector<std::vector<std::optional<double>>>& step_delays()
      const noexcept {
    return step_delays_;
  }
  /// The run's end, its last step's instant (step_count - 1) * step_s.
  [[nodiscard]] double end_s() const noexcept;
  /// The local repairs made (one per lane changed; a dish's once per lane).
  [[nodiscard]] std::size_t repairs() const noexcept { return repairs_; }
  /// The strategy of the flooding lanes (the first lowest-delay one); nullopt without flooding.
  [[nodiscard]] std::optional<std::size_t> flooding_strategy() const noexcept;
  /// Flooding: the next hop node `node` gives towards `target` at `t` from its own table
  /// (network::kNoNode for none), as the walks ask it. Precondition: flooding, a step done.
  [[nodiscard]] std::size_t flooding_next_hop(std::size_t node, std::size_t target, double t);

 private:
  std::unique_ptr<State> state_;
  std::vector<Interval> intervals_;
  FailureChecks checks_;
  std::vector<std::vector<std::optional<double>>> step_delays_;
  std::size_t repairs_ = 0;
};

}  // namespace satsim::failures_app
