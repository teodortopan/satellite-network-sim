// The planner loop with a truth and a view (Phase 4 spec 4.5, 4.6; plan design choice 4), on
// hand-made worlds: the same object for both is the Phase 3 engine exactly; a view that still
// shows a failed element makes the plan pick it and the step drop naming it (the first failed
// element along the route); and the openings for the failures program (hold_route, evaluate,
// replan, planning_graph, set_serving) each do what they document.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "fake.hpp"
#include "routing/engine.hpp"
#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace {

using satsim::routing::PlanOutcome;
using satsim::routing::Route;
using satsim::routing_app::DropCause;
using satsim::routing_app::EndpointModel;
using satsim::routing_app::Engine;
using satsim::routing_app::EngineDirection;
using satsim::routing_app::EngineNetwork;
using satsim::routing_app::EngineSettings;
using satsim::routing_app::Lookahead;
using satsim::routing_app::Planner;
using satsim::routing_app::RouteRow;
using satsim::routing_app::ServingRule;
using satsim::routing_app::SiteRef;
using satsim::routing_app::StepGround;
using satsim::routing_app::testing::down;
using satsim::routing_app::testing::FakeStep;
using satsim::routing_app::testing::FakeWorld;
using satsim::routing_app::testing::plan_of;
using satsim::routing_app::testing::repeat;
using satsim::routing_app::testing::seen;
using satsim::routing_app::testing::up;
namespace topology = satsim::topology;
using Rows = std::vector<std::vector<RouteRow>>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr std::int64_t kNs = 1'000'000'000;  // Steps are 1 s.
constexpr double kLowerBound = 0.001;        // s, below every delay here.
constexpr double kFibre = 0.004;             // s
constexpr auto kFailed = topology::LinkDownReason::kFailed;

satsim::routing::Strategy strategy(std::string_view name) {
  return std::get<satsim::routing::Strategy>(satsim::routing::parse_strategy(name));
}

struct Config {
  EndpointModel model = EndpointModel::kBestCase;
  std::vector<std::string_view> strategies = {"lowest-delay"};
  std::int64_t replan_steps = 4;
  std::int64_t slot_steps = 5;
  Planner planner = Planner::kStart;
  ServingRule serving = ServingRule::kHighestElevation;
  bool backup = false;
};

const std::vector<EngineDirection> kOneWay = {
    {.from = 0, .to = 1, .lower_bound_s = kLowerBound, .has_home_pop = true}};

Engine engine(const Config& c, const topology::LinkPlan& plan, std::size_t satellites,
              std::size_t gateways = 0, std::vector<double> end_delays = {}) {
  EngineNetwork network{.model = c.model,
                        .satellite_count = satellites,
                        .city_count = 2,
                        .gateway_count = gateways,
                        .plan = &plan,
                        .directions = kOneWay,
                        .end_delays_s = std::move(end_delays)};
  EngineSettings settings{.strategies = {},
                          .step_ns = kNs,
                          .replan_ns = c.replan_steps * kNs,
                          .slot_ns = c.slot_steps * kNs,
                          .planner = c.planner,
                          .serving = c.serving,
                          .backup = c.backup,
                          .hop_delay_s = 0.0};
  for (const std::string_view name : c.strategies) {
    settings.strategies.push_back(strategy(name));
  }
  return {std::move(network), std::move(settings)};
}

double sum_s(std::initializer_list<double> km) {
  double s = 0.0;
  for (const double x : km) {
    s += x / kC;
  }
  return s;
}

// Every step of `truth` through step(k, truth, view, rows), the view taken from `view`.
Rows run_views(Engine& e, const FakeWorld& truth, const FakeWorld& view) {
  Rows all;
  for (std::size_t k = 0; k < truth.size(); ++k) {
    std::vector<RouteRow> rows;
    const FakeWorld::View t = truth.view(k);
    const FakeWorld::View v = view.view(k);
    const auto error = e.step(static_cast<std::int64_t>(k), t, v, rows);
    EXPECT_FALSE(error.has_value()) << error.value_or("");
    all.push_back(std::move(rows));
  }
  e.finish();
  return all;
}

// Best case, the engine tests' square. Satellites 0..3, links 0: 0-1 and 1: 1-2 (1000 km),
// 2: 0-3 and 3: 3-2 (1500 km). City 0 sees satellite 0, city 1 sees satellite 2 (600 km).
// Nodes: satellites 0..3, open dish of city 0 = 4, receiving dish of city 1 = 7.
const std::vector<std::pair<std::size_t, std::size_t>> kSquare = {{0, 1}, {1, 2}, {0, 3}, {3, 2}};
constexpr std::size_t kDish0 = 4;
constexpr std::size_t kReceiving1 = 7;
const Route kVia1 = {kDish0, 0, 1, 2, kReceiving1};
const Route kVia3 = {kDish0, 0, 3, 2, kReceiving1};

FakeStep square_step() {
  return FakeStep{.links = {up(1000.0), up(1000.0), up(1500.0), up(1500.0)},
                  .cities = {{seen(0, 50.0, 600.0)}, {seen(2, 50.0, 600.0)}},
                  .gateways = {},
                  .north = {}};
}

// The overlay of a failed satellite (design choice 3): marked down, out of every visible list,
// its links failed.
void fail_satellite(FakeStep& s, std::size_t satellite, std::size_t satellites,
                    const std::vector<std::pair<std::size_t, std::size_t>>& links) {
  s.satellite_down.resize(satellites, 0);
  s.satellite_down[satellite] = 1;
  for (std::size_t i = 0; i < links.size(); ++i) {
    if (links[i].first == satellite || links[i].second == satellite) {
      s.links[i] = down(kFailed);
    }
  }
  for (auto* lists : {&s.cities, &s.gateways}) {
    for (auto& list : *lists) {
      std::erase_if(list,
                    [&](const topology::VisibleSatellite& v) { return v.satellite == satellite; });
    }
  }
}

// Realistic, the engine tests' world. Satellites 0..3; links 0: 0-3 (1200 km), 1: 1-3
// (900 km). City 0 sees satellite 0 (60 deg, 550 km) at steps 0-1, 1 (45 deg, 650 km) at
// steps 0-9 and 2 (30 deg, 800 km) always; the gateway sees 2 (700 km) and 3 (600 km). Nodes:
// satellites 0..3, open dishes 4 and 6, gateway 8, end 9, serving dishes 10 and 11.
const std::vector<std::pair<std::size_t, std::size_t>> kRealLinks = {{0, 3}, {1, 3}};
constexpr std::size_t kGatewayNode = 8;
constexpr std::size_t kEndNode = 9;
constexpr std::size_t kServing0 = 10;

std::vector<FakeStep> realistic_steps(std::size_t n) {
  std::vector<FakeStep> steps;
  for (std::size_t k = 0; k < n; ++k) {
    FakeStep s{.links = {up(1200.0), up(900.0)},
               .cities = {{}, {seen(3, 50.0, 600.0)}},
               .gateways = {{seen(2, 40.0, 700.0), seen(3, 50.0, 600.0)}},
               .north = {}};
    if (k < 2) {
      s.cities[0].push_back(seen(0, 60.0, 550.0));
    }
    if (k < 10) {
      s.cities[0].push_back(seen(1, 45.0, 650.0));
    }
    s.cities[0].push_back(seen(2, 30.0, 800.0));
    steps.push_back(std::move(s));
  }
  return steps;
}

Engine realistic_engine(const Config& c, const topology::LinkPlan& plan) {
  Config r = c;
  r.model = EndpointModel::kRealistic;
  return engine(r, plan, 4, 1, {kFibre});
}

void expect_same_rows(const Rows& a, const Rows& b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t k = 0; k < a.size(); ++k) {
    ASSERT_EQ(a[k].size(), b[k].size()) << k;
    for (std::size_t i = 0; i < a[k].size(); ++i) {
      const RouteRow& x = a[k][i];
      const RouteRow& y = b[k][i];
      EXPECT_EQ(x.ok, y.ok) << k;
      EXPECT_EQ(x.cause, y.cause) << k;
      EXPECT_EQ(x.responsible, y.responsible) << k;
      EXPECT_EQ(x.delay_s, y.delay_s) << k;
      EXPECT_EQ(x.ideal_s, y.ideal_s) << k;
      EXPECT_EQ(x.route, y.route) << k;
      EXPECT_EQ(x.serving, y.serving) << k;
      EXPECT_EQ(x.plan, y.plan) << k;
      EXPECT_EQ(x.backup_s, y.backup_s) << k;
    }
  }
}

// (i) The truth and the view as one object give the one-look-ahead call's rows, and so do two
// views of the same world (equal contents, separate objects: the view's graph is then built
// on its own). Both models, both planners, every strategy, backup on, with links and serving
// satellites coming and going.
TEST(RoutingEngineViewsTest, OneWorldForTruthAndViewIsThePhaseThreeEngine) {
  struct Case {
    Config config;
    bool realistic = false;
  };
  const std::vector<std::string_view> all = {"lowest-delay", "keep-until-broken", "longest-lived",
                                             "threshold:1ms"};
  for (const Case& c :
       {Case{{.strategies = all, .replan_steps = 3, .backup = true}, false},
        Case{{.strategies = all,
              .replan_steps = 3,
              .planner = Planner::kWholeInterval,
              .backup = true},
             false},
        Case{{.strategies = all,
              .replan_steps = 3,
              .serving = ServingRule::kLongestInView,
              .backup = true},
             true},
        Case{{.strategies = all, .replan_steps = 2, .serving = ServingRule::kWithRoute}, true},
        Case{{.strategies = all,
              .replan_steps = 3,
              .planner = Planner::kWholeInterval,
              .serving = ServingRule::kHighestWholeSlot,
              .backup = true},
             true}}) {
    std::vector<FakeStep> steps = c.realistic ? realistic_steps(14) : repeat(square_step(), 10);
    if (!c.realistic) {
      steps[2].links[1] = down();
      steps[3].links[1] = down();
      steps[6].cities[1] = {seen(3, 30.0, 700.0)};
    } else {
      steps[4].gateways[0].clear();
    }
    const FakeWorld world(steps, 1.0, 3);
    const auto plan = plan_of(c.realistic ? kRealLinks : kSquare);
    const auto make = [&] {
      return c.realistic ? realistic_engine(c.config, plan) : engine(c.config, plan, 4);
    };
    Engine one = make();
    Engine same = make();
    Engine two = make();
    const Rows a = satsim::routing_app::testing::run(one, world);
    Rows b;
    for (std::size_t k = 0; k < world.size(); ++k) {
      std::vector<RouteRow> rows;
      const FakeWorld::View v = world.view(k);
      EXPECT_FALSE(same.step(static_cast<std::int64_t>(k), v, v, rows).has_value());
      b.push_back(std::move(rows));
    }
    same.finish();
    const Rows d = run_views(two, world, world);
    expect_same_rows(a, b);
    expect_same_rows(a, d);
    EXPECT_EQ(one.drop_events().size(), two.drop_events().size());
    EXPECT_EQ(two.checks().total(), 0U);
  }
}

// (ii) A link the view still shows: link 1-2 fails at step 2, the planner learns of it at step
// 4 (plans every 2 steps). The plan at step 2 still takes the route via 1 (the dead link),
// steps 2 and 3 drop with failed_link naming link 1; the plan at step 4 routes around it.
TEST(RoutingEngineViewsTest, AStaleViewPlansOverADeadLinkThenRoutesAround) {
  std::vector<FakeStep> truth = repeat(square_step(), 6);
  std::vector<FakeStep> view = truth;
  for (std::size_t k = 2; k < 6; ++k) {
    truth[k].links[1] = down(kFailed);
    if (k >= 4) {
      view[k].links[1] = down(kFailed);
    }
  }
  const auto plan = plan_of(kSquare);
  Engine e = engine({.replan_steps = 2}, plan, 4);
  Rows rows;
  bool dead_edge_planned = false;
  for (std::size_t k = 0; k < 6; ++k) {
    std::vector<RouteRow> r;
    const FakeWorld tw(truth, 1.0);
    const FakeWorld vw(view, 1.0);
    ASSERT_FALSE(e.step(static_cast<std::int64_t>(k), tw.view(k), vw.view(k), r).has_value());
    if (k == 2) {
      // The plan was made on the view: it still has the dead link; the truth does not.
      const auto edges = e.planning_graph().edges_from(1);
      for (const auto& edge : edges) {
        dead_edge_planned = dead_edge_planned || edge.to == 2;
      }
      for (const auto& edge : e.step_graph().edges_from(1)) {
        EXPECT_NE(edge.to, 2U);
      }
    }
    rows.push_back(std::move(r));
  }
  e.finish();
  EXPECT_TRUE(dead_edge_planned);
  for (std::size_t k = 0; k < 6; ++k) {
    const RouteRow& r = rows[k][0];
    EXPECT_EQ(r.route, k < 4 ? kVia1 : kVia3) << k;
    EXPECT_EQ(r.ok, k < 2 || k >= 4) << k;
  }
  EXPECT_EQ(rows[2][0].plan, PlanOutcome::kKept);
  for (const std::size_t k : {std::size_t{2}, std::size_t{3}}) {
    EXPECT_EQ(rows[k][0].cause, DropCause::kFailedLink) << k;
    EXPECT_EQ(rows[k][0].responsible.link, 1U);
    EXPECT_EQ(rows[k][0].responsible.link_reason, kFailed);
    EXPECT_FALSE(rows[k][0].responsible.satellite.has_value());
  }
  ASSERT_EQ(e.drop_events().size(), 1U);
  EXPECT_EQ(e.drop_events()[0].cause, DropCause::kFailedLink);
  EXPECT_EQ(e.drop_events()[0].steps, 2);
  EXPECT_EQ(e.measures()[0].drop_steps_by_cause[static_cast<std::size_t>(DropCause::kFailedLink)],
            2U);
  EXPECT_EQ(e.checks().total(), 0U);
}

// (ii) The same with satellite 1: its links are failed too, but the satellite is named
// (failed_satellite), not its link, and the whole-interval planner on the stale view also
// keeps it; once the view lacks it the plan routes around.
TEST(RoutingEngineViewsTest, AStaleViewPlansThroughADeadSatelliteThenRoutesAround) {
  for (const Planner planner : {Planner::kStart, Planner::kWholeInterval}) {
    std::vector<FakeStep> truth = repeat(square_step(), 6);
    std::vector<FakeStep> view = truth;
    for (std::size_t k = 2; k < 6; ++k) {
      fail_satellite(truth[k], 1, 4, kSquare);
      if (k >= 4) {
        fail_satellite(view[k], 1, 4, kSquare);
      }
    }
    const auto plan = plan_of(kSquare);
    Engine e = engine({.replan_steps = 2, .planner = planner}, plan, 4);
    const Rows rows = run_views(e, FakeWorld(truth, 1.0, 2), FakeWorld(view, 1.0, 2));
    // whole-interval plans at 0 on steps 0-1 of the view: via 1 also.
    for (std::size_t k = 0; k < 6; ++k) {
      EXPECT_EQ(rows[k][0].route, k < 4 ? kVia1 : kVia3) << k;
      EXPECT_EQ(rows[k][0].ok, k < 2 || k >= 4) << k;
    }
    for (const std::size_t k : {std::size_t{2}, std::size_t{3}}) {
      EXPECT_EQ(rows[k][0].cause, DropCause::kFailedSatellite) << k;
      EXPECT_EQ(rows[k][0].responsible.satellite, 1U);
      EXPECT_FALSE(rows[k][0].responsible.link.has_value());
    }
    EXPECT_EQ(e.checks().total(), 0U);
  }
}

// (iii) Realistic: the view still sees satellite 0, which has failed. At the slot start the
// dish takes it by the view (highest elevation), the plan routes through it, and every step
// of the slot is failed_satellite naming it, not serving_left_view. At the next slot start
// (step 5) the view lacks it too: the dish takes satellite 1 and the route works.
TEST(RoutingEngineViewsTest, AServingSatelliteThatHasFailedIsAFailedSatellite) {
  std::vector<FakeStep> truth = realistic_steps(8);
  std::vector<FakeStep> view = truth;
  for (std::size_t k = 0; k < 8; ++k) {
    fail_satellite(truth[k], 0, 4, kRealLinks);
    if (k >= 5) {
      fail_satellite(view[k], 0, 4, kRealLinks);
    }
  }
  // Satellite 0 stays in the view's sky for the whole first slot.
  for (std::size_t k = 2; k < 5; ++k) {
    view[k].cities[0].insert(view[k].cities[0].begin(), seen(0, 60.0, 550.0));
  }
  const auto plan = plan_of(kRealLinks);
  Engine e = realistic_engine({.replan_steps = 5}, plan);
  const Rows rows = run_views(e, FakeWorld(truth, 1.0), FakeWorld(view, 1.0));
  EXPECT_EQ(rows[0][0].route, (Route{kServing0, 0, 3, kGatewayNode, kEndNode}));
  for (std::size_t k = 0; k < 5; ++k) {
    EXPECT_EQ(rows[k][0].serving, 0U) << k;
    EXPECT_EQ(rows[k][0].cause, DropCause::kFailedSatellite) << k;
    EXPECT_EQ(rows[k][0].responsible.satellite, 0U) << k;
  }
  EXPECT_EQ(rows[5][0].serving, 1U);
  EXPECT_TRUE(rows[5][0].ok);
  EXPECT_EQ(rows[5][0].route, (Route{kServing0, 1, 3, kGatewayNode, kEndNode}));
  EXPECT_EQ(e.checks().total(), 0U);
}

// (iii) Realistic: the gateway fails at step 1; the view learns of it at step 5 (plans every
// 5 steps). Steps 1-4 are failed_gateway naming it; at step 5 there is no other gateway, so
// the plan finds no route: no_gateway, as in Phase 3.
TEST(RoutingEngineViewsTest, AGatewayThatHasFailedIsAFailedGateway) {
  std::vector<FakeStep> truth = realistic_steps(7);
  for (auto& s : truth) {  // Satellite 0 out of the sky: the dish serves through satellite 1.
    s.cities[0].erase(s.cities[0].begin(), s.cities[0].end() - 2);
  }
  std::vector<FakeStep> view = truth;
  for (std::size_t k = 1; k < 7; ++k) {
    truth[k].gateway_down = {1};
    truth[k].gateways[0].clear();
    if (k >= 5) {
      view[k].gateway_down = {1};
      view[k].gateways[0].clear();
    }
  }
  const auto plan = plan_of(kRealLinks);
  Engine e = realistic_engine({.replan_steps = 5}, plan);
  const Rows rows = run_views(e, FakeWorld(truth, 1.0), FakeWorld(view, 1.0));
  EXPECT_TRUE(rows[0][0].ok);
  EXPECT_EQ(rows[0][0].route, (Route{kServing0, 1, 3, kGatewayNode, kEndNode}));
  for (std::size_t k = 1; k < 5; ++k) {
    EXPECT_EQ(rows[k][0].cause, DropCause::kFailedGateway) << k;
    EXPECT_EQ(rows[k][0].responsible.site, (SiteRef{.gateway = true, .index = 0})) << k;
  }
  EXPECT_EQ(rows[5][0].plan, PlanOutcome::kNone);
  EXPECT_EQ(rows[5][0].cause, DropCause::kNoGateway);
  EXPECT_EQ(e.checks().total(), 0U);
}

// (iv) Two failed elements on the route: link 0-1 (link 0) and satellite 2. The link comes
// first along the route (dish, 0, 0-1, 1, 1-2, 2, ...), so it is named.
TEST(RoutingEngineViewsTest, TheFirstFailedElementAlongTheRouteIsNamed) {
  std::vector<FakeStep> truth = repeat(square_step(), 3);
  const std::vector<FakeStep> view = truth;
  for (std::size_t k = 1; k < 3; ++k) {
    fail_satellite(truth[k], 2, 4, kSquare);
    truth[k].links[0] = down(kFailed);
    truth[k].cities[1] = {seen(2, 50.0, 600.0)};  // The dish edge alone stays (not the cause).
  }
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4);
  const Rows rows = run_views(e, FakeWorld(truth, 1.0), FakeWorld(view, 1.0));
  EXPECT_EQ(rows[1][0].route, kVia1);
  EXPECT_EQ(rows[1][0].cause, DropCause::kFailedLink);
  EXPECT_EQ(rows[1][0].responsible.link, 0U);
  // With only the satellite failed, it is the satellite.
  std::vector<FakeStep> only = repeat(square_step(), 2);
  fail_satellite(only[1], 2, 4, kSquare);
  Engine f = engine({}, plan, 4);
  const Rows r2 = run_views(f, FakeWorld(only, 1.0), FakeWorld(repeat(square_step(), 2), 1.0));
  EXPECT_EQ(r2[1][0].cause, DropCause::kFailedSatellite);
  EXPECT_EQ(r2[1][0].responsible.satellite, 2U);
}

// (v) hold_route replaces the held route between plans and clears the cause of a plan that
// found none; lane_route reads it back.
TEST(RoutingEngineViewsTest, HoldRouteReplacesTheHeldRoute) {
  std::vector<FakeStep> steps = repeat(square_step(), 3);
  for (auto& link : steps[0].links) {
    link = down();  // The plan at step 0 finds no route.
  }
  const FakeWorld world(steps, 1.0);
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4);
  std::vector<RouteRow> rows;
  ASSERT_FALSE(e.step(0, world.view(0), rows).has_value());
  EXPECT_EQ(rows[0].cause, DropCause::kNoPath);
  EXPECT_TRUE(e.lane_route(0, 0).empty());
  e.hold_route(0, 0, kVia3);
  EXPECT_EQ(e.lane_route(0, 0), kVia3);
  ASSERT_FALSE(e.step(1, world.view(1), rows).has_value());
  EXPECT_FALSE(rows[0].plan.has_value());
  EXPECT_TRUE(rows[0].ok);
  EXPECT_EQ(rows[0].route, kVia3);
  EXPECT_EQ(rows[0].delay_s, sum_s({600.0, 1500.0, 1500.0, 600.0}));
  // An empty route held: no route, and the stored plan cause is not brought back (the plan
  // that found none was cleared by the route held before).
  e.hold_route(0, 0, {});
  ASSERT_FALSE(e.step(2, world.view(2), rows).has_value());
  EXPECT_FALSE(rows[0].ok);
  EXPECT_TRUE(rows[0].route.empty());
  EXPECT_FALSE(rows[0].cause.has_value());
}

// (v) evaluate: what step decides for a lane, on the given ground and graph, recording nothing.
TEST(RoutingEngineViewsTest, EvaluateDecidesOnTheGivenGroundAndGraph) {
  const std::vector<FakeStep> steps = repeat(square_step(), 1);
  const FakeWorld world(steps, 1.0);
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4);
  std::vector<RouteRow> rows;
  ASSERT_FALSE(e.step(0, world.view(0), rows).has_value());
  const StepGround now = world.view(0).step(0);
  const auto same = e.evaluate(0, 0, now, e.step_graph());
  EXPECT_TRUE(same.ok);
  EXPECT_EQ(same.delay_s, rows[0].delay_s);
  EXPECT_FALSE(same.cause.has_value());

  // The same step with link 1-2 failed and satellite 1 not: failed_link.
  std::vector<FakeStep> failed = steps;
  failed[0].links[1] = down(kFailed);
  const FakeWorld failed_world(failed, 1.0);
  const StepGround truth = failed_world.view(0).step(0);
  satsim::network::Graph graph(e.layout().node_count());
  const std::vector<std::optional<std::size_t>> serving;
  ASSERT_FALSE(e.builder().build(std::span(&truth, 1), serving, 1, false, graph).has_value());
  const auto broken = e.evaluate(0, 0, truth, graph);
  EXPECT_FALSE(broken.ok);
  EXPECT_FALSE(broken.delay_s.has_value());
  EXPECT_EQ(broken.cause, DropCause::kFailedLink);
  EXPECT_EQ(broken.responsible.link, 1U);
  // A link down for a geometric reason: laser_link_down, as in Phase 3.
  failed[0].links[1] = down(topology::LinkDownReason::kNoLineOfSight);
  const FakeWorld geometric_world(failed, 1.0);
  const StepGround geometric = geometric_world.view(0).step(0);
  ASSERT_FALSE(e.builder().build(std::span(&geometric, 1), serving, 1, false, graph).has_value());
  EXPECT_EQ(e.evaluate(0, 0, geometric, graph).cause, DropCause::kLaserLinkDown);
  // Nothing was recorded.
  EXPECT_EQ(e.measures()[0].steps, 1U);
  EXPECT_EQ(e.measures()[0].ok_steps, 1U);
}

// (v) replan between two regular plans: the plan is made now, on the view, and held; the
// planning graph is the view's. From step 1 links 1-2 and 0-3 have failed; the view knows only
// of 1-2, so the plan takes the route via 3 (on the truth there would be none), which then
// drops at step 2 on the failed link 0-3.
TEST(RoutingEngineViewsTest, ReplanPlansNowOnTheView) {
  std::vector<FakeStep> truth = repeat(square_step(), 3);
  std::vector<FakeStep> view = truth;
  for (std::size_t k = 1; k < 3; ++k) {
    truth[k].links[1] = down(kFailed);
    truth[k].links[2] = down(kFailed);
    view[k].links[1] = down(kFailed);
  }
  const FakeWorld tw(truth, 1.0);
  const FakeWorld vw(view, 1.0);
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4);  // Plans at steps 0 and 4.
  std::vector<RouteRow> rows;
  ASSERT_FALSE(e.step(0, tw.view(0), vw.view(0), rows).has_value());
  ASSERT_FALSE(e.step(1, tw.view(1), vw.view(1), rows).has_value());
  EXPECT_EQ(rows[0].cause, DropCause::kFailedLink);
  ASSERT_FALSE(e.replan(1, tw.view(1), vw.view(1), rows).has_value());
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].plan, PlanOutcome::kReplaced);
  EXPECT_EQ(rows[0].route, kVia3);
  EXPECT_EQ(e.lane_route(0, 0), kVia3);
  for (const auto& edge : e.planning_graph().edges_from(1)) {
    EXPECT_NE(edge.to, 2U);
  }
  EXPECT_EQ(e.measures()[0].plans, 2U);
  EXPECT_EQ(e.measures()[0].steps, 2U);  // The re-plan recorded no step.
  ASSERT_FALSE(e.step(2, tw.view(2), vw.view(2), rows).has_value());
  EXPECT_FALSE(rows[0].plan.has_value());
  EXPECT_FALSE(rows[0].ok);
  EXPECT_EQ(rows[0].route, kVia3);
  EXPECT_EQ(rows[0].cause, DropCause::kFailedLink);
  EXPECT_EQ(rows[0].responsible.link, 2U);
}

// (v) set_serving gives a dish another serving satellite between slot starts (local repair);
// serving_of reads it back, the next step's graph and rows use it.
TEST(RoutingEngineViewsTest, SetServingChangesTheDishsSatelliteBetweenSlotStarts) {
  const FakeWorld world(realistic_steps(3), 1.0);
  const auto plan = plan_of(kRealLinks);
  Engine e = realistic_engine({.replan_steps = 5}, plan);
  std::vector<RouteRow> rows;
  ASSERT_FALSE(e.step(0, world.view(0), rows).has_value());
  EXPECT_EQ(e.serving_of(0), 0U);
  e.set_serving(0, 1);
  EXPECT_EQ(e.serving_of(0), 1U);
  const Route via1 = {kServing0, 1, 3, kGatewayNode, kEndNode};
  e.hold_route(0, 0, via1);
  ASSERT_FALSE(e.step(1, world.view(1), rows).has_value());
  EXPECT_EQ(rows[0].serving, 1U);
  EXPECT_TRUE(rows[0].ok);
  EXPECT_EQ(rows[0].route, via1);
  EXPECT_EQ(rows[0].delay_s, sum_s({650.0, 900.0, 600.0}) + kFibre);
  e.set_serving(0, std::nullopt);
  ASSERT_FALSE(e.step(2, world.view(2), rows).has_value());
  EXPECT_EQ(rows[0].cause, DropCause::kNoCoverage);
}

}  // namespace
