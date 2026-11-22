// The flooding reaction mode on the loop tests' hand-made world (loop_test.cpp): floods worked
// out by hand (learning instants, messages), against the lowest of the single-detector
// results; view graphs; transitions of a direction through a black hole, loops and a dead end
// to its settled route, every interval's instants written out; an "up again" fact; the
// realistic dish picking again; no failure equal to the baseline; each new check made to fire.
//
// The world (best case unless said otherwise), on the failure tests' small network: links 0:
// 0-1 (1000 km), 1: 1-2 (1500 km), 2: 0-4 (2000 km), 3: 4-5 (1000 km), 4: 1-5 (1000 km),
// 5: 16-17. City A (0) sees satellite 0 at 500 km; city B (1) sees satellites 2 and 5 at
// 500 km (in some tests only 5). Gateway g0 sees satellite 4 at 600 km. 1 s steps, 40 steps
// (the run ends at 39 s), re-plan every 10 s, start planner, lowest-delay, no hop delay;
// detection 0.1 s, per hop 0.001 s, compute 0.1 s.
//
// Control-graph nodes: satellites 0 to 21, dishes A 22 and B 23, gateway g0 24. Best-case
// graph nodes: satellites 0 to 21, A's open dish 22 (and receiving 23), B's receiving dish 25.
// A message over an edge of k km costs hop(k) = k / c + 0.001 s.

#include "failures/flooding.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "failure/fixture.hpp"
#include "failures/control_graph.hpp"
#include "failures/loop.hpp"
#include "failures/settings.hpp"
#include "failures/timeline.hpp"
#include "failures/view.hpp"
#include "fake_geometry.hpp"
#include "inputs/inputs.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/failure/schedule.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/network/many_sources.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/link_state.hpp"
#include "step_world.hpp"

namespace {

using satsim::app::EndpointModel;
using satsim::failure::ElementKind;
using satsim::failure::Failure;
using satsim::failure::testing::failure_at;
using satsim::failures_app::DirectionState;
using satsim::failures_app::FailureLoop;
using satsim::failures_app::Flood;
using satsim::failures_app::Floods;
using satsim::failures_app::Interval;
using satsim::failures_app::LaneKind;
using satsim::failures_app::LaneRow;
using satsim::failures_app::LoopHooks;
using satsim::failures_app::LoopSettings;
using satsim::failures_app::Reaction;
using satsim::failures_app::Status;
using satsim::failures_app::Subject;
using satsim::failures_app::SubjectKind;
using satsim::failures_app::Timeline;
using satsim::failures_app::TimelineSettings;
using satsim::failures_app::testing::FakeGeometry;
using satsim::failures_app::testing::seen;
using satsim::routing_app::DropCause;
using Route = std::vector<std::size_t>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kSteps = 40;
constexpr double kEnd = 39.0;

double hop(double km) { return km / kC + 0.001; }

// A failure at 5 s detected at 5 + 0.1 s and used (or acted on by a realistic dish) 0.1 s
// later, formed as the program forms it.
const double kUses = (5.0 + 0.1) + 0.1;

struct Scene {
  FakeGeometry geometry;
  EndpointModel model;
  satsim::failures_app::ReactionTimes times{.detection_s = 0.1,
                                            .hop_processing_s = 0.001,
                                            .compute_s = 0.1,
                                            .planner_processing_s = 0.0,
                                            .acquisition_s = 30.0,
                                            .repoint = false};
  satsim::routing_app::EngineNetwork network;
  std::vector<Reaction> reactions = {Reaction::kFlooding};
  TimelineSettings settings;
  Timeline timeline;
  Floods floods;
  std::unique_ptr<FailureLoop> loop;
  std::vector<std::vector<LaneRow>> rows;

  explicit Scene(EndpointModel m = EndpointModel::kBestCase) : model(m) {
    geometry.base.position_ecef_km.assign(22, std::nullopt);
    geometry.base.city_visible = {{seen(0, 500.0)}, {seen(2, 500.0), seen(5, 500.0)}};
    geometry.base.gateway_visible = {{seen(4, 600.0)}};
    geometry.set_length(0, 1, 1000.0);
    geometry.set_length(1, 2, 1500.0);
    geometry.set_length(0, 4, 2000.0);
    geometry.set_length(4, 5, 1000.0);
    geometry.set_length(1, 5, 1000.0);
    geometry.set_length(16, 17, 1000.0);
    network.model = m;
    network.satellite_count = 22;
    network.city_count = 2;
    network.gateway_count = 1;
    network.directions = {{.from = 0, .to = 1, .lower_bound_s = 0.001, .has_home_pop = true}};
    network.end_delays_s = {0.001};
  }

  // B sees only satellite 5.
  void b_sees_only_5() { geometry.base.city_visible[1] = {seen(5, 500.0)}; }

  void build(const std::vector<Failure>& failures,
             const satsim::failures_app::AfterFlood& after_flood = {}) {
    settings = TimelineSettings{.times = times,
                                .model = model,
                                .hop_delay_s = 0.0,
                                .step_s = 1.0,
                                .step_count = kSteps,
                                .gateway_count = network.gateway_count,
                                .atmosphere_km = 80.0};
    timeline = satsim::failures_app::build_timeline(
        satsim::failure::testing::small().classification, satsim::failure::testing::small_plan(),
        satsim::failure::FailureSchedule{.failures = failures}, settings, geometry);
    floods = satsim::failures_app::flood_facts(timeline, settings, geometry, after_flood);
  }

  void run(const std::vector<Failure>& failures, const LoopHooks& hooks = {}) {
    build(failures);
    run_built(hooks);
  }

  void run_built(const LoopHooks& hooks = {}) {
    const LoopSettings loop_settings{
        .engine =
            satsim::routing_app::EngineSettings{.strategies = {satsim::routing::Strategy{}},
                                                .step_ns = 1'000'000'000,
                                                .replan_ns = 10'000'000'000,
                                                .slot_ns = 10'000'000'000,
                                                .planner = satsim::routing_app::Planner::kStart,
                                                .serving = {},
                                                .backup = false,
                                                .hop_delay_s = 0.0},
        .reactions = reactions,
        .replan_on_report = false,
        .times = times,
        .step_count = kSteps,
        .floods = &floods};
    loop = std::make_unique<FailureLoop>(timeline, network, loop_settings, hooks);
    satsim::failures_app::testing::GeometryWorld world(geometry, timeline);
    rows.clear();
    for (std::int64_t k = 0; k < kSteps; ++k) {
      world.at(k);
      std::vector<LaneRow> step_rows;
      const auto error = loop->step(k, world, step_rows);
      ASSERT_FALSE(error.has_value()) << error.value_or("");
      rows.push_back(std::move(step_rows));
    }
    loop->finish();
  }

  [[nodiscard]] std::size_t engine_of(LaneKind kind) const {
    for (std::size_t e = 0; e < loop->engine_count(); ++e) {
      if (loop->kind(e) == kind) {
        return e;
      }
    }
    ADD_FAILURE() << "no lane";
    return 0;
  }

  [[nodiscard]] std::vector<Interval> lane(LaneKind kind, std::size_t d = 0) const {
    std::vector<Interval> result;
    for (const Interval& iv : loop->intervals()) {
      if (iv.engine == engine_of(kind) && iv.direction == d) {
        result.push_back(iv);
      }
    }
    return result;
  }
};

struct Expect {
  double start_s;
  double end_s;
  DirectionState state;
  Route route;
  std::optional<Subject> element = std::nullopt;
  std::optional<DropCause> cause = std::nullopt;
};

void expect_intervals(const std::vector<Interval>& got, const std::vector<Expect>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(got[i].start_s, want[i].start_s);
    EXPECT_EQ(got[i].end_s, want[i].end_s);
    EXPECT_EQ(got[i].status.state, want[i].state);
    EXPECT_EQ(got[i].status.route, want[i].route);
    EXPECT_EQ(got[i].status.element, want[i].element);
    if (want[i].cause) {
      EXPECT_EQ(got[i].status.cause, want[i].cause);
    }
  }
}

Subject satellite(std::size_t i) { return {.kind = SubjectKind::kSatellite, .index = i}; }
Subject link(std::size_t i) { return {.kind = SubjectKind::kLink, .index = i}; }

constexpr std::size_t kA = 22;  // A's open dish (graph) and dish (control graph).
constexpr std::size_t kB = 25;  // B's receiving dish (graph).
constexpr std::size_t kDishB = 23;
constexpr std::size_t kG0 = 24;
const Route kA015B = {kA, 0, 1, 5, kB};
const Route kA012B = {kA, 0, 1, 2, kB};
const Route kA045B = {kA, 0, 4, 5, kB};

// The flood of the fact a satellite failure gives its first link. Satellite 1 fails at 5 s:
// facts "satellite 1 down" (nobody on the ground sees 1: no detector, no flood), then "link 0
// down" (0-1, detected by 0), "link 1 down" (1-2, by 2) and "link 4 down" (1-5, by 5), each at
// 5.1 s. The messages graph at 5.1 s has the links 0-4, 4-5 and 16-17 (1 is dead), A-0, B-2,
// B-5 and g0-4. Link 0's flood from 0 (start delay 0):
//   0 at 5.1; A at + hop(500); 4 at + hop(2000); g0 at + (hop(2000) + hop(600));
//   5 at + (hop(2000) + hop(1000)); B at + ((hop(2000) + hop(1000)) + hop(500));
//   2 at + (((hop(2000) + hop(1000)) + hop(500)) + hop(500)); nobody else.
// Messages: 0 sends on its 2 edges; A on none (its one edge is where it came from); 4 on 2 of
// 3; g0 none; 5 on 1 of 2; B on 1 of 2; 2 on none: 6, to 7 nodes.
TEST(FloodingTest, ASatelliteFailureFloodsByHand) {
  Scene scene;
  scene.build({failure_at(ElementKind::kSatellite, 1, 5.0)});
  ASSERT_EQ(scene.timeline.facts.size(), 4U);
  const Flood& none = scene.floods.facts.at(0);
  EXPECT_TRUE(none.learn_s.empty());
  EXPECT_EQ(none.nodes, 0U);
  EXPECT_FALSE(none.last_s.has_value());
  EXPECT_EQ(none.messages, 0U);

  const Flood& f = scene.floods.facts.at(1);
  ASSERT_EQ(f.learn_s.size(), 25U);
  const double t0 = 5.1;
  std::vector<double> want(25, kInf);
  want[0] = t0;
  want[kA] = t0 + (0.0 + hop(500.0));
  want[4] = t0 + (0.0 + hop(2000.0));
  want[kG0] = t0 + ((0.0 + hop(2000.0)) + hop(600.0));
  want[5] = t0 + ((0.0 + hop(2000.0)) + hop(1000.0));
  want[kDishB] = t0 + (((0.0 + hop(2000.0)) + hop(1000.0)) + hop(500.0));
  want[2] = t0 + ((((0.0 + hop(2000.0)) + hop(1000.0)) + hop(500.0)) + hop(500.0));
  for (std::size_t v = 0; v < want.size(); ++v) {
    EXPECT_EQ(f.learn_s[v], want[v]) << v;
  }
  EXPECT_EQ(f.nodes, 7U);
  EXPECT_EQ(f.last_s, std::optional<double>(want[2]));
  EXPECT_EQ(f.messages, 6U);
  EXPECT_EQ(scene.floods.unreached, 0U);
}

// A link fact has two detectors. Link 4 (1-5) fails at 5 s; 1 and 5 detect at 5.1 s, both
// starts of delay 0. The messages graph at 5.1 s has every link but 1-5, A-0, B-2, B-5, g0-4.
//   1, 5 at 5.1; 0 at + hop(1000) (from 1); 4 at + hop(1000) (from 5); B at + hop(500);
//   2 at + (hop(500) + hop(500)), through B (shorter than hop(1500) from 1);
//   A at + (hop(1000) + hop(500)); g0 at + (hop(1000) + hop(600)).
// Each is also the lower of the two floods from one detector alone. Messages: 1 and 5 send on
// both their edges (2 + 2), 0 on 2 of 3, 4 on 2 of 3, B on 1 of 2, 2 on 1 of 2, A and g0 on
// none: 10, to 8 nodes.
TEST(FloodingTest, ALinkFailureFloodsFromBothEnds) {
  Scene scene;
  scene.build({failure_at(ElementKind::kLaserLink, 4, 5.0)});
  ASSERT_EQ(scene.timeline.facts.size(), 1U);
  const Flood& f = scene.floods.facts.at(0);
  const double t0 = 5.1;
  std::vector<double> want(25, kInf);
  want[1] = t0;
  want[5] = t0;
  want[0] = t0 + (0.0 + hop(1000.0));
  want[4] = t0 + (0.0 + hop(1000.0));
  want[kDishB] = t0 + (0.0 + hop(500.0));
  want[2] = t0 + ((0.0 + hop(500.0)) + hop(500.0));
  want[kA] = t0 + ((0.0 + hop(1000.0)) + hop(500.0));
  want[kG0] = t0 + ((0.0 + hop(1000.0)) + hop(600.0));
  ASSERT_EQ(f.learn_s.size(), want.size());
  for (std::size_t v = 0; v < want.size(); ++v) {
    EXPECT_EQ(f.learn_s[v], want[v]) << v;
  }
  EXPECT_EQ(f.nodes, 8U);
  EXPECT_EQ(f.messages, 10U);
  // Against the lowest of the floods from each detector alone, on the same messages graph.
  const auto step = satsim::failures_app::geometric_states(scene.geometry, scene.timeline.links, 5);
  const auto truth =
      satsim::failures_app::overlay_truth(scene.geometry.at(5), step, scene.timeline, t0);
  const satsim::network::Graph graph = satsim::failures_app::control_graph(
      scene.timeline.nodes, scene.timeline.links, truth.ground(), EndpointModel::kBestCase, 0.0);
  const std::vector<satsim::network::Start> from1 = {{.node = 1, .delay_s = 0.0}};
  const std::vector<satsim::network::Start> from5 = {{.node = 5, .delay_s = 0.0}};
  const auto a = satsim::network::shortest_paths_from_many(graph, from1, 0.001);
  const auto b = satsim::network::shortest_paths_from_many(graph, from5, 0.001);
  for (std::size_t v = 0; v < want.size(); ++v) {
    EXPECT_EQ(f.learn_s[v], t0 + std::min(a.delay_s[v], b.delay_s[v])) << v;
  }
}

// A detection after the run's end starts no flood: link 4 failing at the last step, 39 s.
TEST(FloodingTest, NoFloodAfterTheRunsEnd) {
  Scene scene;
  scene.build({failure_at(ElementKind::kLaserLink, 4, 39.0)});
  ASSERT_EQ(scene.timeline.facts.size(), 1U);
  EXPECT_TRUE(scene.floods.facts.at(0).learn_s.empty());
  EXPECT_EQ(scene.floods.facts.at(0).nodes, 0U);
}

// The view graph: the superset without what the view rules out, in the same order.
TEST(FloodingTest, ViewGraphLeavesOutWhatTheViewRulesOut) {
  Scene scene;
  scene.build({failure_at(ElementKind::kSatellite, 1, 5.0)});
  const satsim::routing_app::GraphLayout layout(EndpointModel::kBestCase, 22, 2, 0, 0, 0);
  satsim::network::Graph superset(layout.node_count());
  ASSERT_FALSE(superset.add_undirected_edge(0, 1, 1.0).has_value());
  ASSERT_FALSE(superset.add_undirected_edge(4, 5, 2.0).has_value());
  ASSERT_FALSE(superset.add_edge(kA, 0, 3.0).has_value());
  ASSERT_FALSE(superset.add_edge(5, kB, 4.0).has_value());
  ASSERT_FALSE(superset.add_edge(1, kB, 5.0).has_value());
  const auto view_of = [&](const std::vector<std::size_t>& facts) {
    std::vector<double> knowledge(scene.timeline.facts.size(), kInf);
    for (const std::size_t f : facts) {
      knowledge[f] = 0.0;
    }
    return satsim::failures_app::view_at(scene.timeline, knowledge, 0.0);
  };
  const auto edges = [](const satsim::network::Graph& g) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t u = 0; u < g.node_count(); ++u) {
      for (const auto& e : g.edges_from(u)) {
        out.emplace_back(u, e.to);
      }
    }
    return out;
  };
  // Nothing known: the superset exactly.
  const satsim::network::Graph all =
      satsim::failures_app::view_graph(superset, layout, scene.timeline, view_of({}));
  EXPECT_EQ(edges(all), edges(superset));
  // "Link 0-1 down" (fact 1): its edge goes, nothing else.
  const satsim::network::Graph no01 =
      satsim::failures_app::view_graph(superset, layout, scene.timeline, view_of({1}));
  EXPECT_EQ(edges(no01), (std::vector<std::pair<std::size_t, std::size_t>>{
                             {1, kB}, {4, 5}, {5, 4}, {5, kB}, {kA, 0}}));
  // Every link of 1 down (facts 1 to 3): 1 is taken as down, its dish edge goes too.
  const satsim::network::Graph no1 =
      satsim::failures_app::view_graph(superset, layout, scene.timeline, view_of({1, 2, 3}));
  EXPECT_EQ(edges(no1),
            (std::vector<std::pair<std::size_t, std::size_t>>{{4, 5}, {5, 4}, {5, kB}, {kA, 0}}));
}

// A repair link is in the view graph only once known up.
TEST(FloodingTest, ViewGraphBelievesARepairLinkOnlyWhenKnownUp) {
  Timeline t;
  t.links = {{.satellite_a = 0, .satellite_b = 1}, {.satellite_a = 2, .satellite_b = 3}};
  t.planned_count = 1;
  t.nodes = {.satellites = 4, .cities = 0, .gateways = 0};
  t.facts = {{.subject = link(1),
              .down = false,
              .t_s = 3.0,
              .old = false,
              .detectors = {},
              .planner_known_s = kInf,
              .cause = std::nullopt}};
  const satsim::routing_app::GraphLayout layout(EndpointModel::kBestCase, 4, 0, 0, 0, 0);
  satsim::network::Graph superset(layout.node_count());
  ASSERT_FALSE(superset.add_undirected_edge(0, 1, 1.0).has_value());
  ASSERT_FALSE(superset.add_undirected_edge(2, 3, 1.0).has_value());
  const auto unknown = satsim::failures_app::view_at(t, std::vector<double>{kInf}, 0.0);
  EXPECT_EQ(satsim::failures_app::view_graph(superset, layout, t, unknown).edge_count(), 2U);
  const auto known = satsim::failures_app::view_at(t, std::vector<double>{0.0}, 0.0);
  EXPECT_EQ(satsim::failures_app::view_graph(superset, layout, t, known).edge_count(), 4U);
}

// No failure: the flooding lane is the baseline's, one interval.
TEST(FloodingTest, NoFailureIsTheBaseline) {
  Scene scene;
  scene.run({});
  expect_intervals(scene.lane(LaneKind::kFlooding),
                   {{0.0, kEnd, DirectionState::kDelivered, kA015B}});
  expect_intervals(scene.lane(LaneKind::kBaseline),
                   {{0.0, kEnd, DirectionState::kDelivered, kA015B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
  ASSERT_EQ(scene.rows.at(10).size(), 3U);
  EXPECT_EQ(scene.rows.at(10)[2].row.plan, satsim::routing::PlanOutcome::kKept);
  EXPECT_EQ(scene.rows.at(0)[2].row.plan, satsim::routing::PlanOutcome::kReplaced);
}

// A black hole, a loop, then the settled route. B sees only satellite 5. Link 4 (1-5) fails at
// 5 s, on the route A 0 1 5 B; 1 and 5 detect at 5.1 s (and use it from 5.1 + 0.1 s). 0 and 4
// learn one hop later, at 5.1 + hop(1000), and use it 0.1 s after that.
//  - [5, 5.2): nobody on the route uses the fact: 0 sends to 1, 1 to 5 over the dead link: a
//    black hole on link 4, the route A 0 1.
//  - [5.2, 5.2 + hop(1000)): 1 knows; its only way left to B is 1 0 4 5 B (2 leads nowhere: B
//    does not see it), so it sends back to 0, which does not know yet and sends to 1: a loop,
//    A 0 1 0. It lasts exactly the flood's delay from 1 to 0.
//  - Then 0 knows: 0 4 5 B, and 4 (which uses the fact at the same instant) and 5 agree:
//    delivered A 0 4 5 B, the reference's route from its plan at 10 s.
TEST(FloodingTest, BlackHoleThenLoopThenSettled) {
  Scene scene;
  scene.b_sees_only_5();
  scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)});
  const double knows0 = (5.1 + (0.0 + hop(1000.0))) + 0.1;
  ASSERT_EQ(scene.floods.facts.at(0).learn_s.at(0), 5.1 + (0.0 + hop(1000.0)));
  ASSERT_EQ(scene.floods.facts.at(0).learn_s.at(4), 5.1 + (0.0 + hop(1000.0)));
  expect_intervals(
      scene.lane(LaneKind::kFlooding),
      {{0.0, 5.0, DirectionState::kDelivered, kA015B},
       {5.0, 5.1 + 0.1, DirectionState::kBlackHole, {kA, 0, 1}, link(4), DropCause::kFailedLink},
       {5.1 + 0.1, knows0, DirectionState::kLoop, {kA, 0, 1, 0}},
       {knows0, kEnd, DirectionState::kDelivered, kA045B}});
  expect_intervals(scene.lane(LaneKind::kReference),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, 10.0, DirectionState::kBlackHole, kA015B, link(4)},
                    {10.0, kEnd, DirectionState::kDelivered, kA045B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
  // The rows at 10 s: the plan keeps the walk of 9 s.
  EXPECT_EQ(scene.rows.at(10)[2].row.plan, satsim::routing::PlanOutcome::kKept);
  EXPECT_EQ(scene.rows.at(10)[2].row.route, kA045B);
  EXPECT_TRUE(scene.rows.at(10)[2].row.ok);
}

// Two loops and both kinds of dead end. B sees only satellite 5, which fails at 5 s. Facts:
// "satellite 5 down" (detected by B's dish, which has no other edge: it alone learns), "link 3
// down" (4-5, detected by 4) and "link 4 down" (1-5, by 1), each at 5.1 s. With both link
// facts a view takes 5 as down (every planned link of it down), and B is cut off.
//  - [5, 5.2): a black hole on satellite 5, route A 0 1.
//  - [5.2, 5.1 + hop(1000) + 0.1): 1 knows 1-5 is down and turns back to 0 (1 0 4 5 B); 0 does
//    not know yet: a loop, A 0 1 0.
//  - Then 0 knows 1-5 (from 1) but not yet 4-5 (from 4, hop(2000) away): 0 4 5 B; 4 has
//    known 4-5 since 5.2 and turns back to 0 (4 0 1 5 B; it learns 1-5 only at
//    5.1 + (hop(1000) + hop(2000))): a loop, A 0 4 0, until 0 knows 4-5 too at
//    5.1 + hop(2000) + 0.1.
//  - Then 0 has no way to B: a dead end at 0, a black hole with no element, route A 0.
//  - A's dish knows both at 5.1 + (hop(2000) + hop(500)) + 0.1: a dead end at the sending dish,
//    no_route; B sees no working satellite: no_coverage. The reference has the same from 10 s.
TEST(FloodingTest, TwoLoopsAndDeadEnds) {
  Scene scene;
  scene.b_sees_only_5();
  scene.run({failure_at(ElementKind::kSatellite, 5, 5.0)});
  ASSERT_EQ(scene.timeline.facts.size(), 3U);
  EXPECT_EQ(scene.floods.facts.at(0).nodes, 1U);
  EXPECT_EQ(scene.floods.facts.at(0).learn_s.at(kDishB), 5.1);
  EXPECT_EQ(scene.floods.facts.at(0).messages, 0U);
  const double loop1_end = (5.1 + (0.0 + hop(1000.0))) + 0.1;
  const double loop2_end = (5.1 + (0.0 + hop(2000.0))) + 0.1;
  const double dish_knows = (5.1 + ((0.0 + hop(2000.0)) + hop(500.0))) + 0.1;
  ASSERT_LT(loop1_end, loop2_end);
  ASSERT_LT(loop2_end, dish_knows);
  expect_intervals(
      scene.lane(LaneKind::kFlooding),
      {{0.0, 5.0, DirectionState::kDelivered, kA015B},
       {5.0,
        kUses,
        DirectionState::kBlackHole,
        {kA, 0, 1},
        satellite(5),
        DropCause::kFailedSatellite},
       {kUses, loop1_end, DirectionState::kLoop, {kA, 0, 1, 0}},
       {loop1_end, loop2_end, DirectionState::kLoop, {kA, 0, 4, 0}},
       {loop2_end, dish_knows, DirectionState::kBlackHole, {kA, 0}},
       {dish_knows, kEnd, DirectionState::kNoRoute, {}, std::nullopt, DropCause::kNoCoverage}});
  EXPECT_FALSE(scene.lane(LaneKind::kFlooding)[4].status.cause.has_value());
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// "Up again" floods too. Link 4 fails at 5 s for 10 s (acquisition 2 s): in force again from
// 17 s, when its ends know. After 5.2 s the walk is A 0 1 2 B (1 knows; 2 B is its way); at
// 17.1 s 1 uses the "up" fact and goes back to A 0 1 5 B.
TEST(FloodingTest, AnUpAgainFactFloods) {
  Scene scene;
  scene.times.acquisition_s = 2.0;
  scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0, 10.0)});
  ASSERT_EQ(scene.timeline.facts.size(), 2U);
  const Flood& up = scene.floods.facts.at(1);
  EXPECT_EQ(up.learn_s.at(1), 17.0);
  EXPECT_EQ(up.learn_s.at(5), 17.0);
  EXPECT_EQ(up.learn_s.at(0), 17.0 + (0.0 + hop(1000.0)));
  EXPECT_EQ(up.nodes, 8U);
  expect_intervals(scene.lane(LaneKind::kFlooding),
                   {{0.0, 5.0, DirectionState::kDelivered, kA015B},
                    {5.0, kUses, DirectionState::kBlackHole, {kA, 0, 1}, link(4)},
                    {kUses, 17.0 + 0.1, DirectionState::kDelivered, kA012B},
                    {17.0 + 0.1, kEnd, DirectionState::kDelivered, kA015B}});
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Realistic: city A's dish serves from satellite 0 (it sees 0 and 4); gateway g0 sees 5,
// g1 sees 4 (as loop_test's realistic world). Satellite 0 fails at 5 s. The dish knows at
// 5 + 0.1 s and picks again at 5.1 + 0.1 s by its rule among what A sees, 0 left out:
// satellite 4, and 4's table takes 4 g1. Before that its uplink goes to the dead satellite: a
// black hole on 0 with the route of the dish alone. Graph nodes: satellites 0-21, open and
// receiving dishes 22-27, gateways 28 and 29, ends 30 and 31, serving dishes 32-34.
TEST(FloodingTest, RealisticDishPicksAgain) {
  Scene scene(EndpointModel::kRealistic);
  scene.geometry.base.city_visible = {{seen(0, 500.0), seen(4, 500.0)}, {}, {}};
  scene.geometry.base.gateway_visible = {{seen(5, 500.0)}, {seen(4, 900.0)}};
  scene.network.city_count = 3;
  scene.network.gateway_count = 2;
  scene.network.directions = {{.from = 0, .to = 1, .lower_bound_s = 0.001, .has_home_pop = true},
                              {.from = 0, .to = 2, .lower_bound_s = 0.001, .has_home_pop = true}};
  scene.network.end_delays_s = {0.001, 0.001, 0.001, 0.001};
  scene.run({failure_at(ElementKind::kSatellite, 0, 5.0)});
  for (std::size_t d = 0; d < 2; ++d) {
    SCOPED_TRACE(d);
    const std::size_t end = 30 + d;
    const auto lane = scene.lane(LaneKind::kFlooding, d);
    expect_intervals(
        lane,
        {{0.0, 5.0, DirectionState::kDelivered, {32, 0, 1, 5, 28, end}},
         {5.0, kUses, DirectionState::kBlackHole, {32}, satellite(0), DropCause::kFailedSatellite},
         {kUses, kEnd, DirectionState::kDelivered, {32, 4, 29, end}}});
    ASSERT_EQ(lane.size(), 3U);
    EXPECT_EQ(lane[0].serving, std::optional<std::size_t>(0));
    EXPECT_EQ(lane[2].serving, std::optional<std::size_t>(4));
  }
  EXPECT_EQ(scene.loop->checks().total(), 0U);
}

// Each new check of spec 6.1 for flooding, made to fire.
TEST(FloodingTest, ChecksFire) {
  const auto flooding = [](const Scene& s) { return s.engine_of(LaneKind::kFlooding); };
  {
    // A table using a fact before its node's learning instant plus the compute time.
    Scene scene;
    LoopHooks hooks;
    hooks.flood_use = [](std::size_t, std::size_t, double t) { return t - 0.05; };
    scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().table_fact_early, 0U);
  }
  {
    // A loop that does not pass a node twice; a black hole on a working satellite.
    Scene scene;
    scene.b_sees_only_5();
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e != flooding(scene)) {
        return;
      }
      if (status.state == DirectionState::kLoop) {
        status.route = {kA, 0, 1};
      } else if (status.state == DirectionState::kBlackHole && status.element) {
        status.element = satellite(0);
      }
    };
    scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().loop_not_repeated, 0U);
    EXPECT_GT(scene.loop->checks().black_hole_not_failed, 0U);
  }
  {
    // A dead-end black hole at a node that has a next hop.
    Scene scene;
    scene.b_sees_only_5();
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e == flooding(scene) && status.state == DirectionState::kBlackHole && !status.element) {
        status.route = {kA};
      }
    };
    scene.run({failure_at(ElementKind::kSatellite, 5, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().black_hole_not_failed, 0U);
  }
  {
    // Once settled, a route that is not the reference's, and from whose nodes the tables give
    // another.
    Scene scene;
    scene.b_sees_only_5();
    LoopHooks hooks;
    hooks.before_checks = [&](std::size_t e, std::size_t, std::size_t, Status& status) {
      if (e == flooding(scene) && status.route == kA045B) {
        status.route = {kA, 0, 4, 5, 1, kB};
      }
    };
    scene.run({failure_at(ElementKind::kLaserLink, 4, 5.0)}, hooks);
    EXPECT_GT(scene.loop->checks().settled_differs, 0U);
    EXPECT_GT(scene.loop->checks().settled_tables_differ, 0U);
  }
  {
    // A flood that misses a node the messages graph joins to a detector.
    Scene scene;
    scene.build({failure_at(ElementKind::kLaserLink, 4, 5.0)},
                [](std::size_t, Flood& f) { f.learn_s.at(kA) = kInf; });
    EXPECT_EQ(scene.floods.unreached, 1U);
  }
}

}  // namespace
