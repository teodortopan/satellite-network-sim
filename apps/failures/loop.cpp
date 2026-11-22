#include "failures/loop.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "failures/flooding.hpp"
#include "failures/geometry.hpp"
#include "failures/settings.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/path.hpp"
#include "satsim/network/paths_to.hpp"
#include "satsim/network/shortest_path.hpp"
#include "satsim/routing/forwarding.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace satsim::failures_app {

namespace {

using routing_app::DropCause;
using routing_app::Engine;
using routing_app::GraphLayout;
using routing_app::NodeKind;
using routing_app::NodeMeaning;
using routing_app::StepGround;
using topology::VisibleSatellite;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNsPerS = 1e9;

bool marked(std::span<const char> marks, std::size_t i) noexcept {
  return i < marks.size() && marks[i] != 0;
}

bool is_failure_cause(DropCause cause) noexcept {
  return cause == DropCause::kFailedSatellite || cause == DropCause::kFailedLink ||
         cause == DropCause::kFailedGateway;
}

// The status of what Engine::evaluate (or Engine::step) decided.
Status status_of(bool ok, std::optional<double> delay_s, std::optional<DropCause> cause,
                 const routing_app::Responsible& r, const routing::Route& route) {
  Status s;
  s.route = route;
  if (ok) {
    s.state = DirectionState::kDelivered;
    s.delay_s = delay_s;
    return s;
  }
  s.cause = cause;
  s.state = DirectionState::kNoRoute;
  if (cause && is_failure_cause(*cause)) {
    s.state = DirectionState::kBlackHole;
    if (*cause == DropCause::kFailedSatellite && r.satellite) {
      s.element = Subject{.kind = SubjectKind::kSatellite, .index = *r.satellite};
    } else if (*cause == DropCause::kFailedLink && r.link) {
      s.element = Subject{.kind = SubjectKind::kLink, .index = *r.link};
    } else if (*cause == DropCause::kFailedGateway && r.site) {
      s.element = Subject{.kind = SubjectKind::kGateway, .index = r.site->index};
    }
  }
  return s;
}

// A Lookahead on a StepWorld: offset j is the overlay of step k + j, either the truth with the
// force at one instant, or a view (the same view at every offset).
class OverlayAhead final : public routing_app::Lookahead {
 public:
  explicit OverlayAhead(const Timeline& timeline) : timeline_(&timeline) {}

  void bind_truth(const StepWorld& world, double t) {
    world_ = &world;
    truth_ = true;
    t_ = t;
    cache_.clear();
  }
  void bind_view(const StepWorld& world, ViewState view) {
    world_ = &world;
    truth_ = false;
    view_ = std::move(view);
    cache_.clear();
  }

  [[nodiscard]] std::size_t known_steps() const override { return world_->known_steps(); }

  [[nodiscard]] StepGround step(std::size_t offset) const override {
    return overlaid(offset).ground();
  }

  [[nodiscard]] const OverlaidStep& overlaid(std::size_t offset) const {
    std::unique_ptr<OverlaidStep>& entry = cache_[offset];
    if (!entry) {
      entry = std::make_unique<OverlaidStep>(
          truth_
              ? overlay_truth(world_->geometry(offset), world_->geometric(offset), *timeline_, t_)
              : overlay_view(world_->geometry(offset), world_->geometric(offset), *timeline_,
                             view_));
    }
    return *entry;
  }

  [[nodiscard]] std::vector<double> times_left_in_view_s(std::size_t site,
                                                         std::span<const VisibleSatellite> visible,
                                                         double horizon_s) const override {
    return world_->times_left_in_view_s(site, visible, horizon_s);
  }
  [[nodiscard]] bool moving_north(std::size_t satellite) const override {
    return world_->moving_north(satellite);
  }

  [[nodiscard]] const ViewState& view() const noexcept { return view_; }

 private:
  const Timeline* timeline_;
  const StepWorld* world_ = nullptr;
  bool truth_ = true;
  double t_ = 0.0;
  ViewState view_;
  mutable std::map<std::size_t, std::unique_ptr<OverlaidStep>> cache_;
};

// What a local repair does, once it is due.
struct RepairPlan {
  double at_s = 0.0;
  bool dish = false;
  /// Index in the route of the repairing node (not a dish repair).
  std::size_t node = 0;
  /// The pass instant, and the dead element's failure instant (the fact's t_s).
  double pass_s = 0.0;
  double failed_s = 0.0;
};

// A lane's repair since the last plan of its engine.
struct LaneRepair {
  bool repaired = false;
  /// The route it was given, kept at a plan whose planning graph still has the dead hop (the
  /// edge from the repairing node to the old route's next node; for a dish, from the dish to
  /// its dead serving satellite).
  std::optional<routing::Route> route = std::nullopt;
  std::optional<std::pair<std::size_t, std::size_t>> dead_hop = std::nullopt;
};

struct DishRepair {
  bool repaired = false;
  /// The satellite it picked, given back at a slot start that picks the dead one again.
  std::optional<std::size_t> serving = std::nullopt;
  std::optional<std::size_t> dead = std::nullopt;
};

bool has_edge(const network::Graph& graph, std::size_t from, std::size_t to) {
  if (from >= graph.node_count()) {
    return false;
  }
  const auto edges = graph.edges_from(from);
  return std::any_of(edges.begin(), edges.end(),
                     [&](const network::Edge& e) { return e.to == to; });
}

// A flooding lane (one per direction, of the flooding strategy).
struct FloodLane {
  /// The traffic's walk now (routing::walk); empty without a walk from the source.
  routing::Route walk;
  /// The walk at the last step instant (the plan column compares with it).
  routing::Route step_walk;
  /// The control nodes whose use of a fact can change the walk: the actors of its nodes.
  std::vector<std::size_t> actors;
  /// What Engine::evaluate named for the last drop it judged.
  routing_app::Responsible responsible;
};

// A flooding dish (realistic).
struct FloodDish {
  /// When it picks again: it knows its serving satellite is dead, plus compute_s.
  std::optional<double> repick_s = std::nullopt;
};

struct Slot {
  LaneKind kind = LaneKind::kBaseline;
  std::unique_ptr<Engine> engine;
  std::vector<routing_app::RouteRow> rows;
  /// Current status and open interval per lane (d * S + s).
  std::vector<Status> status;
  std::vector<std::optional<Interval>> open;
  /// The last closed interval per lane (to merge a zero-length change back).
  std::vector<std::optional<std::size_t>> last_closed;
  // Local repair only.
  network::Graph planning;
  std::vector<LaneRepair> repairs;
  std::vector<DishRepair> dishes;
  /// The truth graph of the current instant, with this engine's serving satellites.
  network::Graph graph;
  // Flooding only: per lane (d * S + s, only the flooding strategy's used) and per dish.
  std::vector<FloodLane> flood;
  std::vector<FloodDish> flood_dishes;
};

}  // namespace

std::string_view name_of(DirectionState state) noexcept {
  switch (state) {
    case DirectionState::kDelivered:
      return "delivered";
    case DirectionState::kBlackHole:
      return "black_hole";
    case DirectionState::kLoop:
      return "loop";
    case DirectionState::kNoRoute:
      return "no_route";
  }
  return "?";
}

std::string_view name_of(LaneKind kind) noexcept {
  switch (kind) {
    case LaneKind::kBaseline:
      return "baseline";
    case LaneKind::kReference:
      return "reference";
    case LaneKind::kPlanner:
      return "planner";
    case LaneKind::kLocalRepair:
      return "local-repair";
    case LaneKind::kFlooding:
      return "flooding";
  }
  return "?";
}

struct FailureLoop::State {
  const Timeline* timeline;
  routing_app::EngineNetwork network;
  LoopSettings settings;
  LoopHooks hooks;
  topology::LinkPlan plan;
  double step_s = 1.0;
  std::int64_t replan_steps = 1;
  std::int64_t slot_steps = 1;
  Knowledge knowledge;
  /// The instants the planner learns a fact (finite, increasing, once each).
  std::vector<double> learning;
  /// (min, max) satellite pair -> the first link of Timeline::links joining them.
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> link_of;
  ViewState empty_view;
  OverlayAhead baseline;
  OverlayAhead truth;
  OverlayAhead view;
  std::vector<Slot> slots;
  /// The baseline's ideal per direction at the current step.
  std::vector<std::optional<double>> baseline_ideal;
  /// Settled instant for the settled check, if any.
  std::optional<double> settled_s;
  const StepWorld* world = nullptr;
  std::int64_t k = 0;
  // Flooding.
  std::optional<std::size_t> flood_strategy;
  std::unique_ptr<FloodKnowledge> flood_knowledge;
  std::unique_ptr<FloodTables> flood_tables;
  ViewState all_in_force;
  /// The current plan's window with every run link in force (the superset network).
  std::vector<OverlaidStep> flood_window;
  /// The window's steps at which a serving dish edge is tested.
  std::size_t flood_serving_steps = 1;
  /// Per city: whether some direction sends from it.
  std::vector<char> sending;
  /// The settled instant of the flooding lanes.
  std::optional<double> flood_settled_s;

  State(const Timeline& tl, routing_app::EngineNetwork net, LoopSettings s, LoopHooks h)
      : timeline(&tl),
        network(std::move(net)),
        settings(std::move(s)),
        hooks(std::move(h)),
        baseline(tl),
        truth(tl),
        view(tl) {}

  [[nodiscard]] std::size_t d_count() const { return network.directions.size(); }
  [[nodiscard]] std::size_t s_count() const { return settings.engine.strategies.size(); }
  [[nodiscard]] bool realistic() const {
    return network.model == routing_app::EndpointModel::kRealistic;
  }
  [[nodiscard]] double step_time(std::int64_t step) const {
    return static_cast<double>(step) * step_s;
  }
  /// Whether lane strategy s of `slot` is one the loop runs (the flooding engine's only its
  /// strategy).
  [[nodiscard]] bool active(const Slot& slot, std::size_t s) const {
    return slot.kind != LaneKind::kFlooding || flood_strategy == s;
  }
};

FailureLoop::FailureLoop(const Timeline& timeline, routing_app::EngineNetwork network,
                         LoopSettings settings, LoopHooks hooks)
    : state_(std::make_unique<State>(timeline, std::move(network), std::move(settings),
                                     std::move(hooks))) {
  State& st = *state_;
  st.plan.links = timeline.links;
  st.network.plan = &st.plan;
  st.step_s = static_cast<double>(st.settings.engine.step_ns) / kNsPerS;
  st.replan_steps = std::max<std::int64_t>(
      1, st.settings.engine.replan_ns / std::max<std::int64_t>(1, st.settings.engine.step_ns));
  st.slot_steps = std::max<std::int64_t>(
      1, st.settings.engine.slot_ns / std::max<std::int64_t>(1, st.settings.engine.step_ns));
  st.knowledge = planner_knowledge(timeline);
  if (st.hooks.planner_knowledge) {
    st.hooks.planner_knowledge(st.knowledge);
  }
  for (const double t : st.knowledge) {
    if (std::isfinite(t)) {
      st.learning.push_back(t);
    }
  }
  std::sort(st.learning.begin(), st.learning.end());
  st.learning.erase(std::unique(st.learning.begin(), st.learning.end()), st.learning.end());
  for (std::size_t i = 0; i < timeline.links.size(); ++i) {
    st.link_of.emplace(std::minmax(timeline.links[i].satellite_a, timeline.links[i].satellite_b),
                       i);
  }
  st.empty_view = view_at(timeline, Knowledge(timeline.facts.size(), kInfinity), 0.0);

  std::vector<LaneKind> kinds = {LaneKind::kBaseline, LaneKind::kReference};
  for (const Reaction r : st.settings.reactions) {
    if (r == Reaction::kPlanner) {
      kinds.push_back(LaneKind::kPlanner);
    } else if (r == Reaction::kLocalRepair) {
      kinds.push_back(LaneKind::kLocalRepair);
    } else if (r == Reaction::kFlooding) {
      for (std::size_t s = 0; s < st.s_count() && !st.flood_strategy; ++s) {
        if (st.settings.engine.strategies[s].kind == routing::StrategyKind::kLowestDelay) {
          st.flood_strategy = s;
        }
      }
      // Without lowest-delay or floods there is no flooding lane (the program refuses that).
      if (st.flood_strategy && st.settings.floods != nullptr) {
        kinds.push_back(LaneKind::kFlooding);
      } else {
        st.flood_strategy.reset();
      }
    }
  }
  const std::size_t lanes = st.d_count() * st.s_count();
  for (const LaneKind kind : kinds) {
    Slot slot;
    slot.kind = kind;
    slot.engine =
        std::make_unique<Engine>(st.network, st.settings.engine, st.hooks.engine_before_checks);
    slot.status.assign(lanes, Status{});
    slot.open.assign(lanes, std::nullopt);
    slot.last_closed.assign(lanes, std::nullopt);
    slot.repairs.assign(lanes, LaneRepair{});
    slot.dishes.assign(slot.engine->layout().serving_dish_count(), DishRepair{});
    slot.graph = network::Graph(slot.engine->layout().node_count());
    if (kind == LaneKind::kFlooding) {
      slot.flood.assign(lanes, FloodLane{});
      slot.flood_dishes.assign(slot.engine->layout().serving_dish_count(), FloodDish{});
      st.flood_tables = std::make_unique<FloodTables>(timeline, slot.engine->layout());
    }
    st.slots.push_back(std::move(slot));
  }
  step_delays_.assign(st.slots.size() * lanes, {});
  if (st.flood_strategy) {
    st.flood_knowledge = std::make_unique<FloodKnowledge>(
        timeline, *st.settings.floods, st.settings.times.compute_s, st.hooks.flood_use);
    st.all_in_force = all_in_force_view(timeline);
    st.sending.assign(st.network.city_count, 0);
    for (const routing_app::EngineDirection& dir : st.network.directions) {
      st.sending.at(dir.from) = 1;
    }
    // Settled once every fact that is not old has a flood, every change of the truth is made
    // and every node uses all it will learn; then the first regular plan (realistic: also a
    // slot start).
    bool flooded = true;
    for (std::size_t f = 0; f < timeline.facts.size(); ++f) {
      if (!timeline.facts[f].old && st.settings.floods->facts.at(f).learn_s.empty()) {
        flooded = false;
      }
    }
    if (flooded) {
      double last = timeline.changes.empty() ? 0.0 : timeline.changes.back();
      last = std::max(last, st.flood_knowledge->last_use().value_or(0.0));
      const std::int64_t every =
          st.realistic() ? std::lcm(st.replan_steps, st.slot_steps) : st.replan_steps;
      std::int64_t k = first_step_at_or_after(last, st.step_s);
      k = (k + every - 1) / every * every;
      if (k < st.settings.step_count) {
        st.flood_settled_s = st.step_time(k);
      }
    }
  }

  // The settled instant of the settled check: every fact the planner learns, learnt; every
  // change of the truth made; then the first regular plan (realistic: also a slot start).
  bool all_known = true;
  double bound = 0.0;
  for (const Fact& f : timeline.facts) {
    if (!std::isfinite(f.planner_known_s)) {
      all_known = false;
    } else {
      bound = std::max(bound, f.planner_known_s);
    }
  }
  if (!timeline.changes.empty()) {
    bound = std::max(bound, timeline.changes.back());
  }
  if (all_known) {
    const std::int64_t every =
        st.realistic() ? std::lcm(st.replan_steps, st.slot_steps) : st.replan_steps;
    std::int64_t k = first_step_at_or_after(bound, st.step_s);
    k = (k + every - 1) / every * every;
    if (k < st.settings.step_count) {
      st.settled_s = st.step_time(k);
    }
  }
}

FailureLoop::~FailureLoop() = default;

std::size_t FailureLoop::engine_count() const noexcept { return state_->slots.size(); }
LaneKind FailureLoop::kind(std::size_t engine) const { return state_->slots.at(engine).kind; }
const routing_app::Engine& FailureLoop::engine(std::size_t engine) const {
  return *state_->slots.at(engine).engine;
}
const OverlaidStep& FailureLoop::truth() const { return state_->truth.overlaid(0); }
double FailureLoop::end_s() const noexcept {
  return state_->step_time(state_->settings.step_count - 1);
}

namespace {

// The beliefs the facts give an actor that knows each fact at its planner_known_s, at t:
// written apart from view_at, for the check.
bool view_matches_facts(const ViewState& view, const Timeline& timeline, double t) {
  std::map<Subject, std::pair<double, bool>> latest;  // subject -> (t_s, down)
  for (const Fact& f : timeline.facts) {
    if (!(f.planner_known_s <= t)) {
      continue;
    }
    const auto it = latest.find(f.subject);
    if (it == latest.end() || f.t_s >= it->second.first) {
      latest[f.subject] = {f.t_s, f.down};
    }
  }
  const auto belief = [&](SubjectKind kind, std::size_t i) {
    const auto it = latest.find(Subject{.kind = kind, .index = i});
    if (it == latest.end()) {
      return Belief::kNothing;
    }
    return it->second.second ? Belief::kDown : Belief::kUp;
  };
  for (std::size_t i = 0; i < view.links.size(); ++i) {
    if (view.links[i] != belief(SubjectKind::kLink, i)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < view.satellites.size(); ++i) {
    if (view.satellites[i] != belief(SubjectKind::kSatellite, i)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < view.gateways.size(); ++i) {
    if (view.gateways[i] != belief(SubjectKind::kGateway, i)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// Everything the loop does at one instant, on the State.
class LoopStep {
 public:
  LoopStep(FailureLoop::State& st, std::vector<Interval>& intervals, FailureChecks& checks,
           std::size_t& repairs)
      : st_(&st), intervals_(&intervals), checks_(&checks), repairs_(&repairs) {}

  [[nodiscard]] std::optional<std::string> at_step(
      std::int64_t k, std::vector<LaneRow>& rows,
      std::vector<std::vector<std::optional<double>>>& delays);
  void finish(double end_s);

 private:
  [[nodiscard]] std::size_t lane(std::size_t d, std::size_t s) const {
    return d * st_->s_count() + s;
  }
  [[nodiscard]] std::vector<std::optional<std::size_t>> serving_list(const Slot& slot) const {
    std::vector<std::optional<std::size_t>> serving;
    for (std::size_t i = 0; i < slot.engine->layout().serving_dish_count(); ++i) {
      serving.push_back(slot.engine->serving_of(i));
    }
    return serving;
  }
  [[nodiscard]] std::optional<std::string> evaluate(Slot& slot, bool rebuild);
  [[nodiscard]] std::optional<std::string> repairs_at(double t);
  [[nodiscard]] std::optional<RepairPlan> repair_plan(const Slot& slot, std::size_t d,
                                                      std::size_t s, double t) const;
  [[nodiscard]] double knowing_instant(const Subject& fact_subject, std::size_t control_node,
                                       double t, double pass_s, double& failed_s) const;
  void after_plan(std::size_t e, double t, bool plan, bool slot_start,
                  std::vector<routing_app::RouteRow>* rows);
  [[nodiscard]] std::optional<std::string> replan_on_report(double t);
  void record(double t, bool at_step);
  void check(std::size_t e, std::size_t d, std::size_t s, const Status& status, double t);
  [[nodiscard]] std::optional<double> next_instant(double after, double before) const;
  void put(std::size_t e, std::size_t d, std::size_t s, const Status& status, double t);

 public:
  // Flooding.
  [[nodiscard]] std::size_t flood_next(Slot& slot, std::size_t node, std::size_t target, double t);

 private:
  [[nodiscard]] std::optional<std::size_t> actor_of(const Slot& slot, std::size_t node) const;
  [[nodiscard]] std::optional<std::string> flood_plan(Slot& slot, std::int64_t k);
  [[nodiscard]] std::optional<std::size_t> flood_pick(Slot& slot, std::size_t dish,
                                                      std::optional<std::size_t> leave_out);
  void flood_schedule(Slot& slot, std::size_t dish, double since);
  void flood_repicks(Slot& slot, double t);
  [[nodiscard]] std::optional<std::string> flood_evaluate(Slot& slot, double t);
  [[nodiscard]] Status flood_status(Slot& slot, std::size_t d, std::size_t s, double t,
                                    const StepGround& now);
  void flood_rows(Slot& slot, bool plan);
  void flood_check(std::size_t e, std::size_t d, std::size_t s, const Status& status, double t);
  void flood_settled(Slot& slot, double t);
  [[nodiscard]] std::optional<double> next_flood_instant(double after, double before) const;

  FailureLoop::State* st_;
  std::vector<Interval>* intervals_;
  FailureChecks* checks_;
  std::size_t* repairs_;
};

std::optional<std::string> LoopStep::evaluate(Slot& slot, bool rebuild) {
  const StepGround now = st_->truth.step(0);
  const network::Graph* graph = &slot.engine->step_graph();
  if (rebuild) {
    if (auto error = slot.engine->builder().build(std::span(&now, 1), serving_list(slot), 1, false,
                                                  slot.graph)) {
      return error;
    }
    graph = &slot.graph;
  }
  for (std::size_t d = 0; d < st_->d_count(); ++d) {
    for (std::size_t s = 0; s < st_->s_count(); ++s) {
      const routing_app::LaneState ls = slot.engine->evaluate(d, s, now, *graph);
      slot.status[lane(d, s)] =
          status_of(ls.ok, ls.delay_s, ls.cause, ls.responsible, slot.engine->lane_route(d, s));
    }
  }
  return std::nullopt;
}

double LoopStep::knowing_instant(const Subject& fact_subject, std::size_t control_node, double t,
                                 double pass_s, double& failed_s) const {
  // The latest "down" fact about the subject that is true by t.
  const Fact* found = nullptr;
  for (const Fact& f : st_->timeline->facts) {
    if (f.subject == fact_subject && f.down && f.t_s <= t &&
        (found == nullptr || f.t_s >= found->t_s)) {
      found = &f;
    }
  }
  if (found == nullptr) {
    return pass_s + st_->settings.times.detection_s;
  }
  failed_s = found->t_s;
  if (found->old) {
    return 0.0;
  }
  for (const Detection& det : found->detectors) {
    if (det.node == control_node) {
      return det.t_s;
    }
  }
  return pass_s + st_->settings.times.detection_s;
}

std::optional<RepairPlan> LoopStep::repair_plan(const Slot& slot, std::size_t d, std::size_t s,
                                                double t) const {
  const Status& status = slot.status[lane(d, s)];
  if (status.state != DirectionState::kBlackHole || !status.element ||
      slot.repairs[lane(d, s)].repaired) {
    return std::nullopt;
  }
  const std::optional<Interval>& open = slot.open[lane(d, s)];
  const double pass_s = open && open->status.same_interval(status) ? open->start_s : t;
  const GraphLayout& l = slot.engine->layout();
  const ControlNodes& nodes = st_->timeline->nodes;
  const routing::Route& route = status.route;
  const Subject& element = *status.element;
  RepairPlan plan;
  plan.pass_s = pass_s;
  plan.failed_s = pass_s;
  const auto node_at = [&](std::size_t node) -> std::optional<std::size_t> {
    const auto it = std::find(route.begin(), route.end(), node);
    return it == route.end() ? std::nullopt
                             : std::optional(static_cast<std::size_t>(it - route.begin()));
  };
  double know = pass_s + st_->settings.times.detection_s;
  if (element.kind == SubjectKind::kSatellite) {
    const std::size_t dish = slot.engine->dish_of(d, s);
    if (st_->realistic() && slot.engine->serving_of(dish) == element.index) {
      if (slot.dishes[dish].repaired) {
        return std::nullopt;
      }
      plan.dish = true;
      double failed_s = pass_s;
      (void)knowing_instant(element, nodes.node_count(), t, pass_s, failed_s);
      plan.failed_s = failed_s;
      know = pass_s + st_->settings.times.detection_s;
    } else {
      const std::optional<std::size_t> at = node_at(l.satellite(element.index));
      if (!at || *at == 0) {
        return std::nullopt;
      }
      plan.node = *at - 1;
      const std::size_t before = route[plan.node];
      const NodeMeaning m = l.meaning(before);
      double failed_s = pass_s;
      (void)knowing_instant(element, nodes.node_count(), t, pass_s, failed_s);
      plan.failed_s = failed_s;
      if (m.kind == NodeKind::kSatellite) {
        const auto link = st_->link_of.find(std::minmax(m.index, element.index));
        if (link != st_->link_of.end()) {
          double ignored = 0.0;
          know = knowing_instant(Subject{.kind = SubjectKind::kLink, .index = link->second},
                                 nodes.satellite(m.index), t, pass_s, ignored);
        }
      } else if (m.kind == NodeKind::kOpenDish) {
        double ignored = 0.0;
        know = knowing_instant(element, nodes.dish(m.index), t, pass_s, ignored);
      }
    }
  } else if (element.kind == SubjectKind::kGateway) {
    const std::optional<std::size_t> at = node_at(l.gateway(element.index));
    if (!at || *at == 0) {
      return std::nullopt;
    }
    plan.node = *at - 1;
    const NodeMeaning m = l.meaning(route[plan.node]);
    double failed_s = pass_s;
    know = knowing_instant(
        element, m.kind == NodeKind::kSatellite ? nodes.satellite(m.index) : nodes.node_count(), t,
        pass_s, failed_s);
    plan.failed_s = failed_s;
  } else {
    if (element.index >= st_->timeline->links.size()) {
      return std::nullopt;
    }
    const topology::PlannedLink& link = st_->timeline->links[element.index];
    std::optional<std::size_t> near;
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
      const std::size_t a = route[i];
      const std::size_t b = route[i + 1];
      if ((a == l.satellite(link.satellite_a) && b == l.satellite(link.satellite_b)) ||
          (a == l.satellite(link.satellite_b) && b == l.satellite(link.satellite_a))) {
        near = i;
        break;
      }
    }
    if (!near) {
      return std::nullopt;
    }
    plan.node = *near;
    double failed_s = pass_s;
    know = knowing_instant(element, nodes.satellite(l.meaning(route[*near]).index), t, pass_s,
                           failed_s);
    plan.failed_s = failed_s;
  }
  plan.at_s = std::max(know, pass_s);
  if (st_->hooks.repair_instant) {
    plan.at_s = st_->hooks.repair_instant(plan.at_s);
  }
  return plan;
}

std::optional<std::string> LoopStep::repairs_at(double t) {
  for (Slot& slot : st_->slots) {
    if (slot.kind != LaneKind::kLocalRepair) {
      continue;
    }
    // Each pass applies every repair due by t; a repaired lane never repairs again before the
    // next plan, so this ends.
    for (bool changed = true; changed;) {
      changed = false;
      const GraphLayout& l = slot.engine->layout();
      for (std::size_t d = 0; d < st_->d_count(); ++d) {
        for (std::size_t s = 0; s < st_->s_count(); ++s) {
          const std::optional<RepairPlan> plan = repair_plan(slot, d, s, t);
          if (!plan || plan->at_s > t) {
            continue;
          }
          if (t < plan->failed_s + st_->settings.times.detection_s || t < plan->pass_s) {
            ++checks_->repair_too_early;
          }
          changed = true;
          const Status status = slot.status[lane(d, s)];
          if (plan->dish) {
            // The dish picks again, the dead one left out, and every lane of its city follows.
            const std::size_t dish = slot.engine->dish_of(d, s);
            const std::size_t city = st_->network.directions[d].from;
            const std::size_t dead = status.element->index;
            DishRepair& dr = slot.dishes[dish];
            dr.repaired = true;
            dr.dead = dead;
            const StepGround now = st_->truth.step(0);
            std::vector<std::vector<VisibleSatellite>> cities(now.city_visible.begin(),
                                                              now.city_visible.end());
            std::erase_if(cities.at(city),
                          [&](const VisibleSatellite& v) { return v.satellite == dead; });
            StepGround seen = now;
            seen.city_visible = cities;
            const std::optional<std::size_t> pick =
                slot.engine->pick_serving(st_->truth, seen, city);
            for (std::size_t d2 = 0; d2 < st_->d_count(); ++d2) {
              if (st_->network.directions[d2].from != city) {
                continue;
              }
              for (std::size_t s2 = 0; s2 < st_->s_count(); ++s2) {
                LaneRepair& lr = slot.repairs[lane(d2, s2)];
                lr.repaired = true;
                if (!pick || slot.engine->lane_route(d2, s2).empty()) {
                  continue;
                }
                const std::size_t source = slot.engine->source_node(d2, s2);
                const network::ShortestPaths paths = network::shortest_paths_avoiding(
                    slot.planning, l.satellite(*pick), std::vector<std::size_t>{dead, source});
                const routing::Route tail = paths.path_to(slot.engine->target_node(d2));
                if (tail.empty()) {
                  continue;
                }
                routing::Route route = {source};
                route.insert(route.end(), tail.begin(), tail.end());
                lr.route = route;
                lr.dead_hop = std::pair{source, l.satellite(dead)};
                slot.engine->hold_route(d2, s2, std::move(route));
                ++*repairs_;
              }
            }
            if (pick) {
              slot.engine->set_serving(dish, pick);
              dr.serving = pick;
            }
            continue;
          }
          LaneRepair& lr = slot.repairs[lane(d, s)];
          lr.repaired = true;
          const routing::Route& old = status.route;
          std::vector<std::size_t> blocked(old.begin(),
                                           old.begin() + static_cast<std::ptrdiff_t>(plan->node));
          blocked.push_back(old[plan->node + 1]);
          const network::ShortestPaths paths =
              network::shortest_paths_avoiding(slot.planning, old[plan->node], blocked);
          const routing::Route backup = paths.path_to(old.back());
          if (backup.empty()) {
            continue;
          }
          routing::Route route(old.begin(), old.begin() + static_cast<std::ptrdiff_t>(plan->node));
          route.insert(route.end(), backup.begin(), backup.end());
          lr.route = route;
          lr.dead_hop = std::pair{old[plan->node], old[plan->node + 1]};
          slot.engine->hold_route(d, s, std::move(route));
          ++*repairs_;
        }
      }
      if (changed) {
        if (auto error = evaluate(slot, true)) {
          return error;
        }
      }
    }
  }
  return std::nullopt;
}

void LoopStep::after_plan(std::size_t e, double t, bool plan, bool slot_start,
                          std::vector<routing_app::RouteRow>* rows) {
  Slot& slot = st_->slots[e];
  if (plan && !view_matches_facts(st_->view.view(), *st_->timeline, t)) {
    ++checks_->fact_used_early;
  }
  if (slot.kind != LaneKind::kLocalRepair) {
    return;
  }
  // A dish's repaired satellite is given back at a slot start that picks the dead one again.
  for (std::size_t i = 0; i < slot.dishes.size(); ++i) {
    DishRepair& dr = slot.dishes[i];
    const bool hold = dr.repaired && dr.dead && slot.engine->serving_of(i) == dr.dead;
    if (hold && slot_start && dr.serving) {
      slot.engine->set_serving(i, dr.serving);
    }
    if (plan && !hold) {
      dr = DishRepair{};
    }
  }
  if (!plan) {
    return;
  }
  // A repaired route is kept at a plan whose planning graph still has the dead hop.
  const network::Graph& planning = slot.engine->planning_graph();
  for (std::size_t d = 0; d < st_->d_count(); ++d) {
    for (std::size_t s = 0; s < st_->s_count(); ++s) {
      LaneRepair& lr = slot.repairs[lane(d, s)];
      if (lr.repaired && lr.route && lr.dead_hop &&
          has_edge(planning, lr.dead_hop->first, lr.dead_hop->second)) {
        slot.engine->hold_route(d, s, *lr.route);
        if (rows != nullptr) {
          rows->at(lane(d, s)).plan = routing::PlanOutcome::kKept;
        }
        continue;
      }
      lr = LaneRepair{};
    }
  }
  slot.planning = slot.engine->planning_graph();
}

std::optional<std::string> LoopStep::replan_on_report(double t) {
  std::vector<routing_app::RouteRow> rows;
  for (std::size_t e = 0; e < st_->slots.size(); ++e) {
    Slot& slot = st_->slots[e];
    if (slot.kind != LaneKind::kPlanner && slot.kind != LaneKind::kLocalRepair) {
      continue;
    }
    if (auto error = slot.engine->replan(st_->k, st_->truth, st_->view, rows)) {
      return error;
    }
    after_plan(e, t, true, false, nullptr);
  }
  return std::nullopt;
}

void LoopStep::check(std::size_t e, std::size_t d, std::size_t s, const Status& status, double t) {
  const Slot& slot = st_->slots[e];
  if (slot.kind == LaneKind::kFlooding && status.state != DirectionState::kDelivered) {
    flood_check(e, d, s, status, t);
    return;
  }
  const StepGround now = st_->truth.step(0);
  const GraphLayout& l = slot.engine->layout();
  const routing_app::EngineDirection& dir = st_->network.directions[d];
  const auto sees = [](std::span<const VisibleSatellite> list, std::size_t sat) {
    return routing_app::find_visible(list, sat) != nullptr;
  };
  if (status.state == DirectionState::kDelivered) {
    bool up = status.route.size() >= 2;
    for (std::size_t i = 0; up && i + 1 < status.route.size(); ++i) {
      const std::size_t a = status.route[i];
      const std::size_t b = status.route[i + 1];
      if (a >= l.node_count() || b >= l.node_count()) {
        up = false;
        break;
      }
      const NodeMeaning ma = l.meaning(a);
      const NodeMeaning mb = l.meaning(b);
      if (ma.kind == NodeKind::kSatellite && mb.kind == NodeKind::kSatellite) {
        const auto it = st_->link_of.find(std::minmax(ma.index, mb.index));
        up = it != st_->link_of.end() && now.links != nullptr &&
             it->second < now.links->states.size() && now.links->states[it->second].up();
      } else if (ma.kind == NodeKind::kOpenDish && mb.kind == NodeKind::kSatellite) {
        up =
            !st_->realistic() && ma.index == dir.from && sees(now.city_visible[ma.index], mb.index);
      } else if (ma.kind == NodeKind::kServingDish && mb.kind == NodeKind::kSatellite) {
        up = st_->realistic() && ma.index == dir.from &&
             slot.engine->serving_of(ma.index) == mb.index &&
             sees(now.city_visible[ma.index], mb.index);
      } else if (ma.kind == NodeKind::kSatellite && mb.kind == NodeKind::kReceivingDish) {
        up = !st_->realistic() && mb.index == dir.to && sees(now.city_visible[mb.index], ma.index);
      } else if (ma.kind == NodeKind::kSatellite && mb.kind == NodeKind::kGateway) {
        up = st_->realistic() && sees(now.gateway_visible[mb.index], ma.index);
      } else if (ma.kind == NodeKind::kGateway && mb.kind == NodeKind::kEnd) {
        up = st_->realistic() && mb.index == d && dir.has_home_pop;
      } else {
        up = false;
      }
    }
    if (!up) {
      ++checks_->delivered_not_up;
    }
    if (!st_->settings.times.repoint) {
      const std::optional<double>& ideal = st_->baseline_ideal.at(d);
      if (!ideal || status.delay_s.value_or(0.0) < *ideal) {
        ++checks_->below_baseline_ideal;
      }
    }
  } else if (status.state == DirectionState::kBlackHole) {
    bool failed = false;
    bool on_route = false;
    if (status.element) {
      const Subject& el = *status.element;
      const auto has = [&](std::size_t node) {
        return std::find(status.route.begin(), status.route.end(), node) != status.route.end();
      };
      switch (el.kind) {
        case SubjectKind::kSatellite:
          failed = marked(now.satellite_down, el.index);
          on_route =
              has(l.satellite(el.index)) ||
              (st_->realistic() && slot.engine->serving_of(slot.engine->dish_of(d, s)) == el.index);
          break;
        case SubjectKind::kGateway:
          failed = marked(now.gateway_down, el.index);
          on_route = has(l.gateway(el.index));
          break;
        case SubjectKind::kLink:
          if (el.index < st_->timeline->links.size() && now.links != nullptr &&
              el.index < now.links->states.size()) {
            failed = now.links->states[el.index].down_reason == topology::LinkDownReason::kFailed;
            const topology::PlannedLink& link = st_->timeline->links[el.index];
            for (std::size_t i = 0; i + 1 < status.route.size(); ++i) {
              const auto pair = std::minmax(status.route[i], status.route[i + 1]);
              if (pair ==
                  std::minmax(l.satellite(link.satellite_a), l.satellite(link.satellite_b))) {
                on_route = true;
              }
            }
          }
          break;
      }
    }
    if (!failed || !on_route) {
      ++checks_->black_hole_not_failed;
    }
  }
}

void LoopStep::put(std::size_t e, std::size_t d, std::size_t s, const Status& status, double t) {
  Slot& slot = st_->slots[e];
  const std::size_t i = lane(d, s);
  std::optional<Interval>& open = slot.open[i];
  const std::optional<std::size_t> serving =
      st_->realistic() ? slot.engine->serving_of(slot.engine->dish_of(d, s)) : std::nullopt;
  if (open && open->status.same_interval(status)) {
    return;
  }
  if (open && open->start_s == t) {
    // A change at the instant the interval opened: it never lasted.
    const std::optional<std::size_t> previous = slot.last_closed[i];
    if (previous && (*intervals_)[*previous].status.same_interval(status) &&
        (*intervals_)[*previous].end_s == t) {
      open = (*intervals_)[*previous];
      intervals_->erase(intervals_->begin() + static_cast<std::ptrdiff_t>(*previous));
      slot.last_closed[i].reset();
      // Indices after it shift; only this lane's last index pointed there.
      for (Slot& other : st_->slots) {
        for (auto& index : other.last_closed) {
          if (index && *index > *previous) {
            --*index;
          }
        }
      }
      return;
    }
    open->status = status;
    open->serving = serving;
    return;
  }
  if (open) {
    open->end_s = t;
    slot.last_closed[i] = intervals_->size();
    intervals_->push_back(*open);
  }
  open = Interval{.engine = e,
                  .direction = d,
                  .strategy = s,
                  .start_s = t,
                  .end_s = t,
                  .status = status,
                  .serving = serving};
}

void LoopStep::record(double t, bool at_step) {
  for (std::size_t e = 0; e < st_->slots.size(); ++e) {
    Slot& slot = st_->slots[e];
    if (slot.kind == LaneKind::kBaseline && !at_step) {
      continue;
    }
    for (std::size_t d = 0; d < st_->d_count(); ++d) {
      for (std::size_t s = 0; s < st_->s_count(); ++s) {
        if (!st_->active(slot, s)) {
          continue;
        }
        Status& status = slot.status[lane(d, s)];
        if (st_->hooks.before_checks) {
          st_->hooks.before_checks(e, d, s, status);
        }
        const std::optional<Interval>& open = slot.open[lane(d, s)];
        const bool starts = !open || !open->status.same_interval(status);
        if (slot.kind != LaneKind::kBaseline && (at_step || starts)) {
          check(e, d, s, status, t);
        }
        put(e, d, s, status, t);
      }
    }
  }
}

std::optional<double> LoopStep::next_instant(double after, double before) const {
  std::optional<double> best;
  const auto offer = [&](double t) {
    if (t > after && t < before && (!best || t < *best)) {
      best = t;
    }
  };
  const std::vector<double>& changes = st_->timeline->changes;
  if (const auto it = std::upper_bound(changes.begin(), changes.end(), after);
      it != changes.end()) {
    offer(*it);
  }
  const bool reacting = std::any_of(st_->slots.begin(), st_->slots.end(), [](const Slot& slot) {
    return slot.kind == LaneKind::kPlanner || slot.kind == LaneKind::kLocalRepair;
  });
  if (st_->settings.replan_on_report && reacting) {
    if (const auto it = std::upper_bound(st_->learning.begin(), st_->learning.end(), after);
        it != st_->learning.end()) {
      offer(*it);
    }
  }
  for (const Slot& slot : st_->slots) {
    if (slot.kind != LaneKind::kLocalRepair) {
      continue;
    }
    for (std::size_t d = 0; d < st_->d_count(); ++d) {
      for (std::size_t s = 0; s < st_->s_count(); ++s) {
        if (const std::optional<RepairPlan> plan = repair_plan(slot, d, s, after)) {
          offer(plan->at_s);
        }
      }
    }
  }
  return best;
}

std::optional<std::size_t> LoopStep::actor_of(const Slot& slot, std::size_t node) const {
  const ControlNodes& nodes = st_->timeline->nodes;
  const NodeMeaning m = slot.engine->layout().meaning(node);
  switch (m.kind) {
    case NodeKind::kSatellite:
      return nodes.satellite(m.index);
    case NodeKind::kOpenDish:
    case NodeKind::kReceivingDish:
    case NodeKind::kServingDish:  // Serving dish i is city i's (with-route is refused).
      return nodes.dish(m.index);
    case NodeKind::kGateway:
      return nodes.gateway(m.index);
    case NodeKind::kEnd:
      break;
  }
  return std::nullopt;
}

std::size_t LoopStep::flood_next(Slot& slot, std::size_t node, std::size_t target, double t) {
  const std::optional<std::size_t> actor = actor_of(slot, node);
  if (!actor) {
    return network::kNoNode;
  }
  FloodKnowledge& knowledge = *st_->flood_knowledge;
  FloodTables& tables = *st_->flood_tables;
  const std::vector<std::size_t>& facts = knowledge.facts_at(*actor, t);
  const NodeMeaning m = slot.engine->layout().meaning(node);
  if (m.kind != NodeKind::kServingDish) {
    return tables.tree(facts, target).next.at(node);
  }
  // A serving dish's one uplink: its serving satellite, if the planning graph would have that
  // edge, its view does not take it as down and it reaches the target.
  const std::optional<std::size_t> serving = slot.engine->serving_of(m.index);
  if (!serving) {
    return network::kNoNode;
  }
  for (std::size_t j = 0; j < st_->flood_serving_steps && j < st_->flood_window.size(); ++j) {
    if (routing_app::find_visible(st_->flood_window[j].city_visible.at(m.index), *serving) ==
        nullptr) {
      return network::kNoNode;
    }
  }
  if (marked(tables.view(facts).satellite_down, *serving)) {
    return network::kNoNode;
  }
  return tables.tree(facts, target).reachable(*serving) ? *serving : network::kNoNode;
}

std::optional<std::string> LoopStep::flood_plan(Slot& slot, std::int64_t k) {
  const StepWorld& world = *st_->world;
  const bool whole = st_->settings.engine.planner == routing_app::Planner::kWholeInterval;
  const std::size_t n =
      whole ? std::min(static_cast<std::size_t>(st_->replan_steps), world.known_steps()) : 1;
  st_->flood_window.clear();
  st_->flood_window.reserve(n);
  for (std::size_t j = 0; j < n; ++j) {
    st_->flood_window.push_back(
        overlay_view(world.geometry(j), world.geometric(j), *st_->timeline, st_->all_in_force));
  }
  std::vector<StepGround> grounds;
  grounds.reserve(n);
  for (const OverlaidStep& step : st_->flood_window) {
    grounds.push_back(step.ground());
  }
  const std::size_t rest =
      whole ? static_cast<std::size_t>(st_->slot_steps - k % st_->slot_steps) : 1;
  network::Graph superset(slot.engine->layout().node_count());
  if (auto error =
          slot.engine->builder().build(grounds, serving_list(slot), rest, false, superset)) {
    return error;
  }
  st_->flood_serving_steps = std::min(n, rest);
  st_->flood_tables->plan(std::move(superset));
  return std::nullopt;
}

std::optional<std::size_t> LoopStep::flood_pick(Slot& slot, std::size_t dish,
                                                std::optional<std::size_t> leave_out) {
  const StepGround now = st_->truth.step(0);
  std::vector<std::vector<VisibleSatellite>> cities(now.city_visible.begin(),
                                                    now.city_visible.end());
  if (leave_out) {
    std::erase_if(cities.at(dish),
                  [&](const VisibleSatellite& v) { return v.satellite == *leave_out; });
  }
  StepGround seen = now;
  seen.city_visible = cities;
  return slot.engine->pick_serving(st_->truth, seen, dish);
}

void LoopStep::flood_schedule(Slot& slot, std::size_t dish, double since) {
  FloodDish& fd = slot.flood_dishes.at(dish);
  fd.repick_s.reset();
  const std::optional<std::size_t> serving = slot.engine->serving_of(dish);
  if (!serving) {
    return;
  }
  // The first failure of it from `since` on that it is still in at the detection instant.
  std::optional<double> knows_s;
  for (const Fact& f : st_->timeline->facts) {
    if (f.subject != Subject{.kind = SubjectKind::kSatellite, .index = *serving} || !f.down ||
        f.t_s < since) {
      continue;
    }
    const double t = f.t_s + st_->settings.times.detection_s;
    if (failure::satellite_down(st_->timeline->schedule, *serving, t) &&
        (!knows_s || t < *knows_s)) {
      knows_s = t;
    }
  }
  if (knows_s) {
    fd.repick_s = *knows_s + st_->settings.times.compute_s;
  }
}

void LoopStep::flood_repicks(Slot& slot, double t) {
  for (std::size_t i = 0; i < slot.flood_dishes.size(); ++i) {
    FloodDish& fd = slot.flood_dishes[i];
    if (!fd.repick_s || *fd.repick_s > t) {
      continue;
    }
    fd.repick_s.reset();
    const std::optional<std::size_t> pick = flood_pick(slot, i, slot.engine->serving_of(i));
    if (pick) {
      slot.engine->set_serving(i, pick);
      flood_schedule(slot, i, t);
    }
  }
}

std::optional<std::string> LoopStep::flood_evaluate(Slot& slot, double t) {
  const StepGround now = st_->truth.step(0);
  if (auto error = slot.engine->builder().build(std::span(&now, 1), serving_list(slot), 1, false,
                                                slot.graph)) {
    return error;
  }
  const std::size_t s = st_->flood_strategy.value_or(0);
  for (std::size_t d = 0; d < st_->d_count(); ++d) {
    slot.status[lane(d, s)] = flood_status(slot, d, s, t, now);
  }
  return std::nullopt;
}

Status LoopStep::flood_status(Slot& slot, std::size_t d, std::size_t s, double t,
                              const StepGround& now) {
  Engine& engine = *slot.engine;
  const GraphLayout& l = engine.layout();
  FloodLane& fl = slot.flood[lane(d, s)];
  const std::size_t source = engine.source_node(d, s);
  const std::size_t target = engine.target_node(d);
  fl.walk.clear();
  fl.actors.clear();
  fl.responsible = {};
  // What the engine names for `held` held as a route, shown as `shown`.
  const auto by_engine = [&](routing::Route held, const routing::Route& shown) {
    engine.hold_route(d, s, std::move(held));
    const routing_app::LaneState ls = engine.evaluate(d, s, now, slot.graph);
    fl.responsible = ls.responsible;
    return status_of(false, std::nullopt, ls.cause, ls.responsible, shown);
  };
  if (st_->realistic() && !st_->network.directions[d].has_home_pop) {
    return by_engine({}, {});  // no_service
  }
  const routing::Walk walk = routing::walk(
      source, target, [&](std::size_t node) { return flood_next(slot, node, target, t); });
  for (const std::size_t node : walk.path) {
    if (const std::optional<std::size_t> actor = actor_of(slot, node)) {
      fl.actors.push_back(*actor);
    }
  }
  std::sort(fl.actors.begin(), fl.actors.end());
  fl.actors.erase(std::unique(fl.actors.begin(), fl.actors.end()), fl.actors.end());
  const routing::Route& path = walk.path;
  if (walk.outcome == routing::WalkOutcome::kDeadEnd && path.size() == 1) {
    // No way on from the source.
    Status status = by_engine({}, {});
    if (!status.cause) {
      status.state = DirectionState::kNoRoute;
      status.cause = st_->realistic() ? DropCause::kNoGateway : DropCause::kNoPath;
    }
    return status;
  }
  fl.walk = path;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const std::size_t u = path[i];
    const std::size_t v = path[i + 1];
    if (has_edge(slot.graph, u, v)) {
      continue;
    }
    // The first hop that is not up: a failed element there, or another reason.
    const NodeMeaning mu = l.meaning(u);
    const NodeMeaning mv = l.meaning(v);
    std::optional<std::pair<DropCause, Subject>> failed;
    if (mv.kind == NodeKind::kSatellite && marked(now.satellite_down, mv.index)) {
      failed = std::pair{DropCause::kFailedSatellite,
                         Subject{.kind = SubjectKind::kSatellite, .index = mv.index}};
    } else if (mv.kind == NodeKind::kGateway && marked(now.gateway_down, mv.index)) {
      failed = std::pair{DropCause::kFailedGateway,
                         Subject{.kind = SubjectKind::kGateway, .index = mv.index}};
    } else if (mu.kind == NodeKind::kSatellite && mv.kind == NodeKind::kSatellite &&
               !marked(now.satellite_down, mu.index)) {
      const auto it = st_->link_of.find(std::minmax(mu.index, mv.index));
      if (it != st_->link_of.end() && now.links != nullptr &&
          it->second < now.links->states.size() &&
          now.links->states[it->second].down_reason == topology::LinkDownReason::kFailed) {
        failed = std::pair{DropCause::kFailedLink,
                           Subject{.kind = SubjectKind::kLink, .index = it->second}};
      }
    }
    if (failed) {
      Status status;
      status.state = DirectionState::kBlackHole;
      status.cause = failed->first;
      status.element = failed->second;
      status.route.assign(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(i + 1));
      return status;
    }
    Status status = by_engine(path, path);
    if (status.cause && is_failure_cause(*status.cause)) {
      // A failure further along: the cause of the walk up to the missing hop.
      status = by_engine(
          routing::Route(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(i + 2)), path);
    }
    status.state = DirectionState::kNoRoute;
    status.element.reset();
    return status;
  }
  Status status;
  status.route = path;
  switch (walk.outcome) {
    case routing::WalkOutcome::kDelivered:
      status.state = DirectionState::kDelivered;
      status.delay_s = network::path_delay_s(slot.graph, path);
      break;
    case routing::WalkOutcome::kDeadEnd:
      status.state = DirectionState::kBlackHole;  // A later node with no next hop.
      break;
    case routing::WalkOutcome::kLoop:
      status.state = DirectionState::kLoop;
      break;
  }
  return status;
}

void LoopStep::flood_rows(Slot& slot, bool plan) {
  const std::size_t lanes = st_->d_count() * st_->s_count();
  slot.rows.assign(lanes, routing_app::RouteRow{});
  const std::vector<routing_app::RouteRow>& reference = st_->slots.at(1).rows;
  for (std::size_t d = 0; d < st_->d_count(); ++d) {
    for (std::size_t s = 0; s < st_->s_count(); ++s) {
      routing_app::RouteRow& row = slot.rows[lane(d, s)];
      row.direction = d;
      row.strategy = s;
      row.ideal_s = reference.at(lane(d, s)).ideal_s;
      if (!st_->active(slot, s)) {
        continue;
      }
      FloodLane& fl = slot.flood[lane(d, s)];
      row.responsible = fl.responsible;
      if (plan) {
        row.plan = fl.walk.empty() ? routing::PlanOutcome::kNone
                                   : (fl.walk == fl.step_walk ? routing::PlanOutcome::kKept
                                                              : routing::PlanOutcome::kReplaced);
      }
      fl.step_walk = fl.walk;
    }
  }
}

void LoopStep::flood_check(std::size_t e, std::size_t d, std::size_t s, const Status& status,
                           double t) {
  Slot& slot = st_->slots[e];
  const GraphLayout& l = slot.engine->layout();
  const StepGround now = st_->truth.step(0);
  const FloodLane& fl = slot.flood[lane(d, s)];
  const routing::Route& route = status.route;
  if (status.state == DirectionState::kLoop) {
    const bool repeated = route.size() >= 2 && std::find(route.begin(), route.end() - 1,
                                                         route.back()) != route.end() - 1;
    if (!repeated) {
      ++checks_->loop_not_repeated;
    }
    return;
  }
  if (status.state != DirectionState::kBlackHole) {
    return;
  }
  const std::size_t target = slot.engine->target_node(d);
  bool ok = false;
  if (status.element) {
    const Subject& el = *status.element;
    // Where the traffic goes after the route.
    const bool walked = !route.empty() && fl.walk.size() > route.size() &&
                        std::equal(route.begin(), route.end(), fl.walk.begin());
    const std::size_t last = walked ? route.back() : network::kNoNode;
    const std::size_t next = walked ? fl.walk[route.size()] : network::kNoNode;
    switch (el.kind) {
      case SubjectKind::kSatellite:
        ok = marked(now.satellite_down, el.index) &&
             ((walked && next == l.satellite(el.index)) ||
              (route.empty() && st_->realistic() &&
               slot.engine->serving_of(slot.engine->dish_of(d, s)) == el.index));
        break;
      case SubjectKind::kGateway:
        ok = marked(now.gateway_down, el.index) && walked && next == l.gateway(el.index);
        break;
      case SubjectKind::kLink:
        if (walked && el.index < st_->timeline->links.size() && now.links != nullptr &&
            el.index < now.links->states.size()) {
          const topology::PlannedLink& link = st_->timeline->links[el.index];
          ok = now.links->states[el.index].down_reason == topology::LinkDownReason::kFailed &&
               std::minmax(last, next) ==
                   std::minmax(l.satellite(link.satellite_a), l.satellite(link.satellite_b));
        }
        break;
    }
  } else {
    // A node with no next hop, recomputed from its table.
    ok = !route.empty() && route.back() != target &&
         flood_next(slot, route.back(), target, t) == network::kNoNode;
  }
  if (!ok) {
    ++checks_->black_hole_not_failed;
  }
}

void LoopStep::flood_settled(Slot& slot, double t) {
  const std::size_t s = st_->flood_strategy.value_or(0);
  for (std::size_t d = 0; d < st_->d_count(); ++d) {
    const Status& a = slot.status[lane(d, s)];
    const Status& b = st_->slots.at(1).status[lane(d, s)];
    if (a.state != b.state || a.route != b.route) {
      ++checks_->settled_differs;
    }
    if (a.state != DirectionState::kDelivered) {
      continue;
    }
    // Following the tables from every node of the route gives the rest of it.
    const std::size_t target = slot.engine->target_node(d);
    for (std::size_t i = 0; i + 1 < a.route.size(); ++i) {
      const routing::Walk rest = routing::walk(
          a.route[i], target, [&](std::size_t node) { return flood_next(slot, node, target, t); });
      if (!std::equal(rest.path.begin(), rest.path.end(),
                      a.route.begin() + static_cast<std::ptrdiff_t>(i), a.route.end())) {
        ++checks_->settled_tables_differ;
        break;
      }
    }
  }
}

std::optional<double> LoopStep::next_flood_instant(double after, double before) const {
  std::optional<double> best;
  const auto offer = [&](double t) {
    if (t > after && t < before && (!best || t < *best)) {
      best = t;
    }
  };
  for (const Slot& slot : st_->slots) {
    if (slot.kind != LaneKind::kFlooding) {
      continue;
    }
    const std::size_t s = st_->flood_strategy.value_or(0);
    for (std::size_t d = 0; d < st_->d_count(); ++d) {
      for (const std::size_t actor : slot.flood[lane(d, s)].actors) {
        if (const std::optional<double> t = st_->flood_knowledge->next_use(actor, after)) {
          offer(*t);
        }
      }
    }
    for (const FloodDish& fd : slot.flood_dishes) {
      if (fd.repick_s) {
        offer(*fd.repick_s);
      }
    }
  }
  return best;
}

std::optional<std::string> LoopStep::at_step(
    std::int64_t k, std::vector<LaneRow>& rows,
    std::vector<std::vector<std::optional<double>>>& delays) {
  const double t = st_->step_time(k);
  const double next = st_->step_time(k + 1);
  const StepWorld& world = *st_->world;
  st_->k = k;
  st_->baseline.bind_view(world, st_->empty_view);
  st_->truth.bind_truth(world, t);
  st_->view.bind_view(world, view_at(*st_->timeline, st_->knowledge, t));
  const bool plan = k % st_->replan_steps == 0;
  const bool slot_start = st_->realistic() && k % st_->slot_steps == 0;
  for (std::size_t e = 0; e < st_->slots.size(); ++e) {
    Slot& slot = st_->slots[e];
    std::optional<std::string> error;
    switch (slot.kind) {
      case LaneKind::kBaseline:
        error = slot.engine->step(k, st_->baseline, st_->baseline, slot.rows);
        break;
      case LaneKind::kReference:
        error = slot.engine->step(k, st_->truth, st_->truth, slot.rows);
        break;
      case LaneKind::kPlanner:
      case LaneKind::kLocalRepair:
        error = slot.engine->step(k, st_->truth, st_->view, slot.rows);
        if (!error && (plan || slot_start)) {
          after_plan(e, t, plan, slot_start, &slot.rows);
        }
        break;
      case LaneKind::kFlooding:
        flood_repicks(slot, t);
        if (slot_start) {
          for (std::size_t i = 0; i < slot.flood_dishes.size(); ++i) {
            if (i < st_->sending.size() && st_->sending[i] != 0) {
              slot.engine->set_serving(i, flood_pick(slot, i, std::nullopt));
              flood_schedule(slot, i, t);
            }
          }
        }
        if (plan) {
          error = flood_plan(slot, k);
        }
        break;
    }
    if (error) {
      return error;
    }
  }
  st_->baseline_ideal.assign(st_->d_count(), std::nullopt);
  for (const routing_app::RouteRow& row : st_->slots[0].rows) {
    st_->baseline_ideal[row.direction] = row.ideal_s;
  }
  // Baseline statuses from its rows; every other engine evaluated (a repair restored at a plan
  // or a serving satellite given back changes what the engine's rows hold).
  for (Slot& slot : st_->slots) {
    if (slot.kind == LaneKind::kBaseline) {
      for (const routing_app::RouteRow& row : slot.rows) {
        slot.status[lane(row.direction, row.strategy)] =
            status_of(row.ok, row.ok ? std::optional(row.delay_s) : std::nullopt, row.cause,
                      row.responsible, row.route);
      }
    } else if (slot.kind == LaneKind::kFlooding) {
      if (auto error = flood_evaluate(slot, t)) {
        return error;
      }
    } else if (auto error = evaluate(slot, slot.kind == LaneKind::kLocalRepair)) {
      return error;
    }
  }
  if (st_->settings.replan_on_report && !plan &&
      std::binary_search(st_->learning.begin(), st_->learning.end(), t)) {
    if (auto error = replan_on_report(t)) {
      return error;
    }
    for (Slot& slot : st_->slots) {
      if (slot.kind == LaneKind::kPlanner || slot.kind == LaneKind::kLocalRepair) {
        if (auto error = evaluate(slot, true)) {
          return error;
        }
      }
    }
  }
  if (auto error = repairs_at(t)) {
    return error;
  }
  for (Slot& slot : st_->slots) {
    if (slot.kind == LaneKind::kFlooding) {
      flood_rows(slot, plan);
    }
  }
  record(t, true);

  // The rows of the step's instant.
  rows.clear();
  const std::size_t lanes = st_->d_count() * st_->s_count();
  for (std::size_t i = 0; i < lanes; ++i) {
    for (std::size_t e = 0; e < st_->slots.size(); ++e) {
      const Slot& slot = st_->slots[e];
      if (!st_->active(slot, i % st_->s_count())) {
        continue;
      }
      LaneRow out{.engine = e, .row = slot.rows.at(i), .status = slot.status[i]};
      routing_app::RouteRow& r = out.row;
      const Status& status = out.status;
      r.ok = status.state == DirectionState::kDelivered;
      r.delay_s = status.delay_s.value_or(0.0);
      r.cause = status.cause;
      r.route = status.route;
      if (st_->realistic()) {
        r.serving = slot.engine->serving_of(slot.engine->dish_of(r.direction, r.strategy));
      }
      delays[e * lanes + i].push_back(r.ok ? std::optional(r.delay_s) : std::nullopt);
      rows.push_back(std::move(out));
    }
  }

  // The settled check, at steps.
  if (st_->settled_s && t >= *st_->settled_s) {
    for (const Slot& slot : st_->slots) {
      if (slot.kind != LaneKind::kPlanner && slot.kind != LaneKind::kLocalRepair) {
        continue;
      }
      for (std::size_t d = 0; d < st_->d_count(); ++d) {
        for (std::size_t s = 0; s < st_->s_count(); ++s) {
          if (st_->settings.engine.strategies[s].kind != routing::StrategyKind::kLowestDelay) {
            continue;
          }
          const Status& a = slot.status[lane(d, s)];
          const Status& b = st_->slots[1].status[lane(d, s)];
          if (a.state != b.state || a.route != b.route) {
            ++checks_->settled_differs;
          }
        }
      }
    }
  }
  if (st_->flood_settled_s && t >= *st_->flood_settled_s) {
    for (Slot& slot : st_->slots) {
      if (slot.kind == LaneKind::kFlooding) {
        flood_settled(slot, t);
      }
    }
  }

  // The instants inside the step.
  // The run ends at its last step's instant: nothing is worked out after it.
  // An instant of the flooding lanes alone changes nothing else.
  double now = t;
  while (k + 1 < st_->settings.step_count) {
    const std::optional<double> others = next_instant(now, next);
    const std::optional<double> flood = next_flood_instant(now, next);
    if (!others && !flood) {
      break;
    }
    const bool flood_only = !others || (flood && *flood < *others);
    now = flood_only ? flood.value_or(now) : others.value_or(now);
    st_->truth.bind_truth(world, now);
    if (!flood_only) {
      const bool learns = st_->settings.replan_on_report &&
                          std::binary_search(st_->learning.begin(), st_->learning.end(), now);
      if (learns) {
        st_->view.bind_view(world, view_at(*st_->timeline, st_->knowledge, now));
        if (auto error = replan_on_report(now)) {
          return error;
        }
      }
      for (Slot& slot : st_->slots) {
        if (slot.kind != LaneKind::kBaseline && slot.kind != LaneKind::kFlooding) {
          if (auto error = evaluate(slot, true)) {
            return error;
          }
        }
      }
      if (auto error = repairs_at(now)) {
        return error;
      }
    }
    for (Slot& slot : st_->slots) {
      if (slot.kind == LaneKind::kFlooding) {
        flood_repicks(slot, now);
        if (auto error = flood_evaluate(slot, now)) {
          return error;
        }
      }
    }
    record(now, false);
  }
  // Leave the truth of the step's instant for truth().
  if (now != t) {
    st_->truth.bind_truth(world, t);
  }
  return std::nullopt;
}

void LoopStep::finish(double end_s) {
  if (st_->flood_knowledge) {
    checks_->table_fact_early = st_->flood_knowledge->early();
  }
  for (Slot& slot : st_->slots) {
    for (std::optional<Interval>& open : slot.open) {
      if (open) {
        if (open->start_s < end_s) {
          open->end_s = end_s;
          intervals_->push_back(*open);
        }
        open.reset();
      }
    }
    slot.engine->finish();
  }
  std::sort(intervals_->begin(), intervals_->end(), [](const Interval& a, const Interval& b) {
    return std::tie(a.direction, a.strategy, a.engine, a.start_s) <
           std::tie(b.direction, b.strategy, b.engine, b.start_s);
  });
}

std::optional<std::string> FailureLoop::step(std::int64_t k, const StepWorld& world,
                                             std::vector<LaneRow>& rows) {
  state_->world = &world;
  LoopStep step(*state_, intervals_, checks_, repairs_);
  return step.at_step(k, rows, step_delays_);
}

void FailureLoop::finish() {
  LoopStep step(*state_, intervals_, checks_, repairs_);
  step.finish(end_s());
}

std::optional<std::size_t> FailureLoop::flooding_strategy() const noexcept {
  return state_->flood_strategy;
}

std::size_t FailureLoop::flooding_next_hop(std::size_t node, std::size_t target, double t) {
  for (Slot& slot : state_->slots) {
    if (slot.kind == LaneKind::kFlooding) {
      LoopStep step(*state_, intervals_, checks_, repairs_);
      return step.flood_next(slot, node, target, t);
    }
  }
  return network::kNoNode;
}

}  // namespace satsim::failures_app
