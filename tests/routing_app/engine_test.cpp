// The planner loop (Phase 3 spec 4.3 to 4.7, 6.1) on hand-made worlds whose every route,
// cause and time is worked out by hand: held routes breaking between plans, the two planners,
// the serving slot and each serving rule, every drop cause and drop events, longest-lived's
// access candidates, the backup path, and each route check made to fire.

#include "routing/engine.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "fake.hpp"
#include "routing/graph.hpp"
#include "routing/settings.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/routing/planner.hpp"
#include "satsim/routing/strategy.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace {

using satsim::routing::Route;
using satsim::routing_app::DropCause;
using satsim::routing_app::DropEvent;
using satsim::routing_app::EndpointModel;
using satsim::routing_app::Engine;
using satsim::routing_app::EngineDirection;
using satsim::routing_app::EngineNetwork;
using satsim::routing_app::EngineSettings;
using satsim::routing_app::Planner;
using satsim::routing_app::RouteRow;
using satsim::routing_app::ServingRule;
using satsim::routing_app::SiteRef;
using satsim::routing_app::testing::down;
using satsim::routing_app::testing::FakeStep;
using satsim::routing_app::testing::FakeWorld;
using satsim::routing_app::testing::plan_of;
using satsim::routing_app::testing::repeat;
using satsim::routing_app::testing::run;
using satsim::routing_app::testing::seen;
using satsim::routing_app::testing::seen_in;
using satsim::routing_app::testing::up;
namespace topology = satsim::topology;
using Rows = std::vector<std::vector<RouteRow>>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr std::int64_t kNs = 1'000'000'000;  // Steps are 1 s.
constexpr double kLowerBound = 0.001;        // s, below every delay here.
constexpr double kFibre = 0.004;             // s

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

Engine engine(const Config& s, const topology::LinkPlan& plan, std::size_t satellites,
              std::vector<EngineDirection> directions, std::size_t gateways = 0,
              std::vector<double> end_delays = {}, satsim::routing_app::BeforeChecks hook = {}) {
  EngineNetwork network{.model = s.model,
                        .satellite_count = satellites,
                        .city_count = 2,
                        .gateway_count = gateways,
                        .plan = &plan,
                        .directions = std::move(directions),
                        .end_delays_s = std::move(end_delays)};
  EngineSettings settings{.strategies = {},
                          .step_ns = kNs,
                          .replan_ns = s.replan_steps * kNs,
                          .slot_ns = s.slot_steps * kNs,
                          .planner = s.planner,
                          .serving = s.serving,
                          .backup = s.backup,
                          .hop_delay_s = 0.0};
  for (const std::string_view name : s.strategies) {
    settings.strategies.push_back(strategy(name));
  }
  return {std::move(network), std::move(settings), std::move(hook)};
}

const std::vector<EngineDirection> kOneWay = {
    {.from = 0, .to = 1, .lower_bound_s = kLowerBound, .has_home_pop = true}};

// Best case. Satellites 0..3, links 0: 0-1 and 1: 1-2 (1000 km each), 2: 0-3 and 3: 3-2
// (1500 km each). City 0 sees satellite 0 and city 1 sees satellite 2, both at 600 km. Nodes:
// satellites 0..3, open dish of city 0 = 4, receiving dish of city 1 = 7.
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

// Route delays summed in route order, as path_delay_s does.
double sum_s(std::initializer_list<double> km) {
  double s = 0.0;
  for (const double x : km) {
    s += x / kC;
  }
  return s;
}

// A held route breaking between re-plans: link 1-2 is down at steps 2 to 5, plans are every 4
// steps. Steps 0, 1 via satellite 1; 2, 3 dropped (laser_link_down, link 1); the plan at 4
// takes the route via 3, held to the end.
TEST(RoutingEngineTest, AHeldRouteBreakingBetweenPlansDrops) {
  std::vector<FakeStep> steps = repeat(square_step(), 8);
  for (std::size_t k = 2; k <= 5; ++k) {
    steps[k].links[1] = down(topology::LinkDownReason::kNoLineOfSight);
  }
  const FakeWorld world(steps, 1.0);
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4, kOneWay);
  const Rows rows = run(e, world);
  for (std::size_t k = 0; k < 8; ++k) {
    const RouteRow& r = rows[k][0];
    const bool dropped = k == 2 || k == 3;
    EXPECT_EQ(r.ok, !dropped) << k;
    EXPECT_EQ(r.route, k < 4 ? kVia1 : kVia3) << k;
    EXPECT_EQ(r.plan.has_value(), k % 4 == 0) << k;
    if (dropped) {
      EXPECT_EQ(r.cause, DropCause::kLaserLinkDown);
      EXPECT_EQ(r.responsible.link, 1U);
      EXPECT_EQ(r.responsible.link_reason, topology::LinkDownReason::kNoLineOfSight);
    } else {
      EXPECT_FALSE(r.cause.has_value()) << k;
    }
  }
  EXPECT_EQ(rows[0][0].delay_s, sum_s({600.0, 1000.0, 1000.0, 600.0}));
  EXPECT_EQ(rows[4][0].delay_s, sum_s({600.0, 1500.0, 1500.0, 600.0}));
  // The ideal at step 4 is via 3 (link 1-2 is down); at step 6 via 1 again.
  EXPECT_EQ(rows[6][0].ideal_s, sum_s({600.0, 1000.0, 1000.0, 600.0}));

  ASSERT_EQ(e.drop_events().size(), 1U);
  const DropEvent& event = e.drop_events()[0];
  EXPECT_EQ(event.start_step, 2);
  EXPECT_EQ(event.steps, 2);
  EXPECT_EQ(event.cause, DropCause::kLaserLinkDown);
  EXPECT_EQ(event.responsible.link, 1U);
  const auto& m = e.measures()[0];
  EXPECT_EQ(m.breaks, 1U);
  EXPECT_EQ(m.recoveries, 1U);
  EXPECT_EQ(m.bad_changes + m.increase_changes, 0U);
  EXPECT_EQ(m.lifetimes_s, (std::vector<double>{2.0, 4.0}));
  EXPECT_EQ(m.drop_steps_by_cause[static_cast<std::size_t>(DropCause::kLaserLinkDown)], 2U);
  EXPECT_EQ(m.drop_events, 1U);
  EXPECT_EQ(m.plans, 2U);
  EXPECT_EQ(m.replaced, 2U);
  EXPECT_EQ(e.checks().total(), 0U);
}

// start against whole-interval: they differ exactly in the interval where link 1-2 goes down
// (steps 0 to 3); whole-interval avoids it from the first plan and never drops.
TEST(RoutingEngineTest, StartAndWholeIntervalDifferWhereALinkDisappears) {
  std::vector<FakeStep> steps = repeat(square_step(), 8);
  for (std::size_t k = 2; k <= 5; ++k) {
    steps[k].links[1] = down();
  }
  const FakeWorld world(steps, 1.0, 4);
  const auto plan = plan_of(kSquare);
  Engine start = engine({}, plan, 4, kOneWay);
  Engine whole = engine({.planner = Planner::kWholeInterval}, plan, 4, kOneWay);
  const Rows a = run(start, world);
  const Rows b = run(whole, world);
  for (std::size_t k = 0; k < 8; ++k) {
    EXPECT_TRUE(b[k][0].ok) << k;
    EXPECT_EQ(b[k][0].route, kVia3) << k;
    const bool same = a[k][0].route == b[k][0].route && a[k][0].ok == b[k][0].ok;
    EXPECT_EQ(same, k >= 4) << k;
  }
  EXPECT_TRUE(whole.drop_events().empty());
  EXPECT_EQ(start.drop_events().size(), 1U);
}

// A ground link of the held route lost: from step 2 city 1 sees satellite 3 instead of 2.
TEST(RoutingEngineTest, LosingTheLastSatelliteIsAGroundLinkLost) {
  std::vector<FakeStep> steps = repeat(square_step(), 4);
  steps[2].cities[1] = {seen(3, 30.0, 700.0)};
  steps[3].cities[1] = {seen(3, 30.0, 700.0)};
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4, kOneWay);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[2][0].cause, DropCause::kGroundLinkLost);
  EXPECT_EQ(rows[2][0].responsible.satellite, 2U);
  EXPECT_EQ(rows[2][0].responsible.site, (SiteRef{.gateway = false, .index = 1}));
  ASSERT_EQ(e.drop_events().size(), 1U);
  EXPECT_EQ(e.drop_events()[0].steps, 2);
}

// No coverage at the sender (step 1) and then at the receiver (step 3): each its own event,
// naming the city; the held route works again in between.
TEST(RoutingEngineTest, NoCoverageNamesTheCityThatSeesNothing) {
  std::vector<FakeStep> steps = repeat(square_step(), 4);
  steps[1].cities[0].clear();
  steps[3].cities[1].clear();
  const auto plan = plan_of(kSquare);
  Engine e = engine({}, plan, 4, kOneWay);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[1][0].cause, DropCause::kNoCoverage);
  EXPECT_EQ(rows[1][0].responsible.site, (SiteRef{.gateway = false, .index = 0}));
  EXPECT_TRUE(rows[2][0].ok);
  EXPECT_EQ(rows[3][0].cause, DropCause::kNoCoverage);
  EXPECT_EQ(rows[3][0].responsible.site, (SiteRef{.gateway = false, .index = 1}));
  EXPECT_EQ(e.drop_events().size(), 2U);
}

// No path at the plan (every link down at step 0) lasts until the next plan, though the links
// are back at step 1.
TEST(RoutingEngineTest, NoPathAtThePlanLastsUntilTheNextPlan) {
  std::vector<FakeStep> steps = repeat(square_step(), 4);
  for (auto& link : steps[0].links) {
    link = down();
  }
  const auto plan = plan_of(kSquare);
  Engine e = engine({.replan_steps = 2}, plan, 4, kOneWay);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[0][0].cause, DropCause::kNoPath);
  EXPECT_EQ(rows[0][0].plan, satsim::routing::PlanOutcome::kNone);
  EXPECT_EQ(rows[1][0].cause, DropCause::kNoPath);
  EXPECT_TRUE(rows[1][0].ideal_s.has_value());  // A route existed; none was planned.
  EXPECT_TRUE(rows[2][0].ok);
  ASSERT_EQ(e.drop_events().size(), 1U);
  EXPECT_EQ(e.drop_events()[0].start_step, 0);
  EXPECT_EQ(e.drop_events()[0].steps, 2);
}

// Realistic. Satellites 0..3; links 0: 0-3 (1200 km), 1: 1-3 (900 km). City 0 sees satellite
// 0 (60 deg, 550 km) at steps 0-1, 1 (45 deg, 650 km) at steps 0-9 and 2 (30 deg, 800 km) at
// every step; the gateway sees 2 (700 km) and 3 (600 km). Routes from city 0's serving dish:
// via 0: 550 + 1200 + 600 km; via 1: 650 + 900 + 600 km; via 2 (bent pipe): 800 + 700 km.
// Nodes: satellites 0..3, open dishes 4 and 6, gateway 8, end 9, serving dishes 10 and 11.
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

Engine realistic_engine(const Config& s, const topology::LinkPlan& plan,
                        satsim::routing_app::BeforeChecks hook = {}) {
  Config r = s;
  r.model = EndpointModel::kRealistic;
  return engine(r, plan, 4, kOneWay, 1, {kFibre}, std::move(hook));
}

// Highest elevation picks satellite 0, which leaves view at step 2, inside the 5-step slot:
// steps 2-4 are serving_left_view. At step 5 the dish takes satellite 1, at step 10 (1 gone)
// satellite 2.
TEST(RoutingEngineTest, AServingSatelliteLeavingViewInsideItsSlot) {
  const auto plan = plan_of(kRealLinks);
  Engine e = realistic_engine({.replan_steps = 5}, plan);
  const Rows rows = run(e, FakeWorld(realistic_steps(20), 1.0));
  for (std::size_t k = 0; k < 20; ++k) {
    const RouteRow& r = rows[k][0];
    const std::size_t serving = k < 5 ? 0 : (k < 10 ? 1 : 2);
    EXPECT_EQ(r.serving, serving) << k;
    EXPECT_EQ(r.ok, k < 2 || k >= 5) << k;
  }
  EXPECT_EQ(rows[0][0].route, (Route{kServing0, 0, 3, kGatewayNode, kEndNode}));
  EXPECT_EQ(rows[12][0].route, (Route{kServing0, 2, kGatewayNode, kEndNode}));
  EXPECT_EQ(rows[0][0].delay_s, sum_s({550.0, 1200.0, 600.0}) + kFibre);
  EXPECT_EQ(rows[3][0].cause, DropCause::kServingLeftView);
  EXPECT_EQ(rows[3][0].responsible.satellite, 0U);
  ASSERT_EQ(e.drop_events().size(), 1U);
  EXPECT_EQ(e.drop_events()[0].start_step, 2);
  EXPECT_EQ(e.drop_events()[0].steps, 3);
  EXPECT_EQ(e.drop_events()[0].cause, DropCause::kServingLeftView);
  // The ideal lets the dish use any satellite: the bent pipe through 2, below every route.
  EXPECT_EQ(rows[0][0].ideal_s, sum_s({800.0, 700.0}) + kFibre);
  EXPECT_EQ(e.checks().total(), 0U);
}

// Each serving rule at the first slot start, worked out by hand. Times left in view from step
// 0: satellite 0 2 s, 1 10 s, 2 20 s (the world's end).
//  - highest-elevation: 0 (60 deg).
//  - longest-in-view: 2 (20 s).
//  - highest-whole-slot (5 s): 1 and 2 stay the whole slot; the higher is 1.
//  - with-route (lowest-delay): the shortest route from an open dish is the bent pipe via 2.
TEST(RoutingEngineTest, EachServingRulePicksTheSatelliteWorkedOutByHand) {
  const auto plan = plan_of(kRealLinks);
  const FakeWorld world(realistic_steps(20), 1.0);
  for (const auto& [rule, expected] : {std::pair{ServingRule::kHighestElevation, std::size_t{0}},
                                       std::pair{ServingRule::kLongestInView, std::size_t{2}},
                                       std::pair{ServingRule::kHighestWholeSlot, std::size_t{1}},
                                       std::pair{ServingRule::kWithRoute, std::size_t{2}}}) {
    Engine e = realistic_engine({.replan_steps = 5, .serving = rule}, plan);
    const Rows rows = run(e, world);
    EXPECT_EQ(rows[0][0].serving, expected) << satsim::routing_app::name_of(rule);
    EXPECT_TRUE(rows[0][0].ok) << satsim::routing_app::name_of(rule);
    EXPECT_EQ(rows[0][0].route[1], expected);
    EXPECT_EQ(e.checks().total(), 0U);
  }
}

// with-route follows the lane's own strategy: keep-until-broken keeps its route's satellite at
// the next slot start while it is up, lowest-delay switches as soon as a better one appears.
TEST(RoutingEngineTest, WithRouteFollowsEachStrategy) {
  const auto plan = plan_of(kRealLinks);
  std::vector<FakeStep> steps = realistic_steps(10);
  // Satellite 2 is out of view in the first slot, so both start through satellite 1.
  for (std::size_t k = 0; k < 5; ++k) {
    steps[k].cities[0].pop_back();
  }
  Engine e = realistic_engine({.strategies = {"lowest-delay", "keep-until-broken"},
                               .replan_steps = 5,
                               .serving = ServingRule::kWithRoute},
                              plan);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[0][0].serving, 1U);
  EXPECT_EQ(rows[0][1].serving, 1U);
  EXPECT_EQ(rows[5][0].serving, 2U);  // The bent pipe is now the best.
  EXPECT_EQ(rows[5][1].serving, 1U);  // Its route via 1 still works.
  EXPECT_EQ(e.checks().total(), 0U);
}

// No gateway at the plan (the gateway sees nothing in the first slot): steps 0, 1 no_gateway
// naming the serving satellite; from step 2 its serving satellite has left (serving_left_view
// comes first). A sender with no home PoP is no_service at every step.
TEST(RoutingEngineTest, NoGatewayAndNoService) {
  const auto plan = plan_of(kRealLinks);
  std::vector<FakeStep> steps = realistic_steps(6);
  for (std::size_t k = 0; k < 5; ++k) {
    steps[k].gateways[0].clear();
  }
  Config s{.model = EndpointModel::kRealistic, .replan_steps = 5};
  std::vector<EngineDirection> directions = kOneWay;
  directions.push_back({.from = 1, .to = 0, .lower_bound_s = kLowerBound, .has_home_pop = false});
  Engine e = engine(s, plan, 4, directions, 1, {kFibre, std::nan("")});
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[0][0].cause, DropCause::kNoGateway);
  EXPECT_EQ(rows[0][0].responsible.satellite, 0U);
  EXPECT_EQ(rows[1][0].cause, DropCause::kNoGateway);
  EXPECT_EQ(rows[2][0].cause, DropCause::kServingLeftView);
  EXPECT_TRUE(rows[5][0].ok);
  for (std::size_t k = 0; k < 6; ++k) {
    EXPECT_EQ(rows[k][1].cause, DropCause::kNoService) << k;
    EXPECT_FALSE(rows[k][1].ideal_s.has_value());
  }
  EXPECT_EQ(e.checks().total(), 0U);
}

// Every drop cause, each produced by one of the worlds above, and never a dropped step
// without a cause.
TEST(RoutingEngineTest, EveryDropCauseIsProducedAndEveryDropHasOne) {
  std::set<DropCause> causes;
  std::size_t without = 0;
  const auto collect = [&](const Engine& e, const Rows& rows) {
    for (const auto& step : rows) {
      for (const RouteRow& r : step) {
        if (r.cause) {
          causes.insert(*r.cause);
        }
      }
    }
    without += e.checks().drop_without_cause;
  };
  {
    const auto plan = plan_of(kSquare);
    std::vector<FakeStep> steps = repeat(square_step(), 12);
    for (auto& link : steps[0].links) {
      link = down();  // no_path at the first plan.
    }
    steps[5].links[1] = down();                // laser_link_down.
    steps[6].cities[1] = {seen(3, 1.0, 1.0)};  // ground_link_lost.
    steps[7].cities[0].clear();                // no_coverage.
    Engine e = engine({.replan_steps = 4}, plan, 4, kOneWay);
    collect(e, run(e, FakeWorld(steps, 1.0)));
  }
  {
    const auto plan = plan_of(kRealLinks);
    std::vector<FakeStep> steps = realistic_steps(6);
    steps[0].gateways[0].clear();  // no_gateway; serving_left_view at steps 2-4.
    std::vector<EngineDirection> directions = kOneWay;
    directions.push_back({.from = 1, .to = 0, .lower_bound_s = kLowerBound, .has_home_pop = false});
    Engine e = engine({.model = EndpointModel::kRealistic, .replan_steps = 5}, plan, 4, directions,
                      1, {kFibre, std::nan("")});
    collect(e, run(e, FakeWorld(steps, 1.0)));
  }
  EXPECT_EQ(causes.size(), satsim::routing_app::kAllDropCauses.size());
  EXPECT_EQ(without, 0U);
}

// Longest-lived, best case. City 0 sees satellites 0 (north, 2 s left), 1 (north, 6 s), 2
// (south, 3 s) and 4 (south, 5 s); city 1 sees 5 (north, 6 s), 6 (south, 4 s), 7 (north, 1 s).
// Candidates: heads 1 (north) and 4 (south), tails 5 and 6. Links: 0-5 500 km (the shortest
// route), 1-5 2500, 1-6 3000, 4-6 2000; 4-5 none. The lowest-delay joined pair is 4-6.
TEST(RoutingEngineTest, LongestLivedTakesTheLongestLivedCandidatesOfEachDirection) {
  const std::vector<std::pair<std::size_t, std::size_t>> links = {{0, 5}, {1, 5}, {1, 6}, {4, 6}};
  const auto plan = plan_of(links);
  std::vector<FakeStep> steps;
  for (std::size_t k = 0; k < 6; ++k) {
    FakeStep s{.links = {up(500.0), up(2500.0), up(3000.0), up(2000.0)},
               .cities = {{}, {}},
               .gateways = {},
               .north = {0, 1, 5, 7}};
    const auto add = [&](std::size_t city, std::size_t satellite, std::size_t steps_in_view) {
      if (k < steps_in_view) {
        s.cities[city].push_back(seen(satellite, 40.0, 600.0));
      }
    };
    add(0, 0, 2);
    add(0, 1, 6);
    add(0, 2, 3);
    add(0, 4, 5);
    add(1, 5, 6);
    add(1, 6, 4);
    add(1, 7, 1);
    steps.push_back(std::move(s));
  }
  // Nodes: satellites 0..7, open dish of city 0 = 8, receiving dish of city 1 = 11.
  Engine e = engine({.strategies = {"lowest-delay", "longest-lived"}, .replan_steps = 6}, plan, 8,
                    kOneWay);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_EQ(rows[0][0].route, (Route{8, 0, 5, 11}));
  EXPECT_EQ(rows[0][1].route, (Route{8, 4, 6, 11}));
  EXPECT_EQ(rows[0][1].delay_s, sum_s({600.0, 2000.0, 600.0}));
  // lowest-delay's route breaks at step 2 (satellite 0 leaves); longest-lived's at step 4.
  EXPECT_EQ(rows[2][0].cause, DropCause::kGroundLinkLost);
  EXPECT_TRUE(rows[3][1].ok);
  EXPECT_EQ(rows[4][1].cause, DropCause::kGroundLinkLost);
  EXPECT_EQ(e.checks().total(), 0U);
}

// Longest-lived takes its candidates per shell, because lasers never join two shells. Shell 0:
// satellites 0 (north, 3 s left) and 1 (south, 5 s) over city 0, 4 (north, 6 s) and 5 (south,
// 2 s) over city 1. Shell 1: 2 (north, 6 s) and 3 (south, 2 s) over city 0, 6 (north, 3 s) and
// 7 (south, 5 s) over city 1. Links only within a shell and a direction of travel: 0-4 1000 km,
// 1-5 1500, 2-6 2000, 3-7 800.
//  - One candidate per direction overall would give heads 2 (shell 1) and 1 (shell 0), tails 4
//    (shell 0) and 7 (shell 1): no pair is joined, so no route.
//  - Per shell: heads 0, 1, 2, 3 and tails 4, 5, 6, 7; the joined pairs are 0-4, 1-5, 2-6 and
//    3-7, and the lowest delay is 3-7: 600 + 800 + 600 km.
TEST(RoutingEngineTest, LongestLivedTakesItsCandidatesInEveryShell) {
  const std::vector<std::pair<std::size_t, std::size_t>> links = {{0, 4}, {1, 5}, {2, 6}, {3, 7}};
  const auto plan = plan_of(links);
  std::vector<FakeStep> steps;
  for (std::size_t k = 0; k < 6; ++k) {
    FakeStep s{.links = {up(1000.0), up(1500.0), up(2000.0), up(800.0)},
               .cities = {{}, {}},
               .gateways = {},
               .north = {0, 2, 4, 6}};
    const auto add = [&](std::size_t city, std::size_t shell, std::size_t satellite,
                         std::size_t steps_in_view) {
      if (k < steps_in_view) {
        s.cities[city].push_back(seen_in(shell, satellite, 40.0, 600.0));
      }
    };
    add(0, 0, 0, 3);
    add(0, 0, 1, 5);
    add(0, 1, 2, 6);
    add(0, 1, 3, 2);
    add(1, 0, 4, 6);
    add(1, 0, 5, 2);
    add(1, 1, 6, 3);
    add(1, 1, 7, 5);
    steps.push_back(std::move(s));
  }
  // Nodes: satellites 0..7, open dish of city 0 = 8, receiving dish of city 1 = 11.
  Engine e = engine({.strategies = {"longest-lived"}, .replan_steps = 6}, plan, 8, kOneWay);
  const Rows rows = run(e, FakeWorld(steps, 1.0));
  EXPECT_TRUE(rows[0][0].ok);
  EXPECT_EQ(rows[0][0].route, (Route{8, 3, 7, 11}));
  EXPECT_EQ(rows[0][0].delay_s, sum_s({600.0, 800.0, 600.0}));
  EXPECT_EQ(e.checks().total(), 0U);
}

// The backup path shares no satellite with the held route. Best case: satellites 0-1-2 carry
// the held route, 4-5 the backup. Realistic: the serving satellite is not removed.
TEST(RoutingEngineTest, TheBackupPathSharesNoSatelliteWithTheHeldRoute) {
  {
    const std::vector<std::pair<std::size_t, std::size_t>> links = {{0, 1}, {1, 2}, {4, 5}};
    const auto plan = plan_of(links);
    const FakeStep s{.links = {up(1000.0), up(1000.0), up(3000.0)},
                     .cities = {{seen(0, 50.0, 600.0), seen(4, 40.0, 700.0)},
                                {seen(2, 50.0, 600.0), seen(5, 40.0, 700.0)}},
                     .gateways = {},
                     .north = {}};
    Engine e = engine({.backup = true}, plan, 6, kOneWay);
    const Rows rows = run(e, FakeWorld(repeat(s, 5), 1.0));
    // Nodes: satellites 0..5, open dish of city 0 = 6, receiving dish of city 1 = 9.
    EXPECT_EQ(rows[0][0].route, (Route{6, 0, 1, 2, 9}));
    EXPECT_EQ(rows[0][0].backup_s, sum_s({700.0, 3000.0, 700.0}));
    EXPECT_FALSE(rows[1][0].backup_s.has_value());  // Not a re-plan instant.
    EXPECT_EQ(rows[4][0].backup_s, sum_s({700.0, 3000.0, 700.0}));
    EXPECT_EQ(e.measures()[0].backup_ms.size(), 2U);
  }
  {
    // The realistic world plus link 0-2: the held route via 0-3, the backup via 0-2 keeps the
    // serving satellite 0.
    const std::vector<std::pair<std::size_t, std::size_t>> links = {{0, 3}, {1, 3}, {0, 2}};
    const auto plan = plan_of(links);
    std::vector<FakeStep> steps = realistic_steps(2);
    for (auto& s : steps) {
      s.links.push_back(up(2000.0));
    }
    Engine e = realistic_engine({.replan_steps = 5, .backup = true}, plan);
    const Rows rows = run(e, FakeWorld(steps, 1.0));
    EXPECT_EQ(rows[0][0].route, (Route{kServing0, 0, 3, kGatewayNode, kEndNode}));
    EXPECT_EQ(rows[0][0].backup_s, sum_s({550.0, 2000.0, 700.0}) + kFibre);
  }
}

// Each route check of spec 6.1 fails when a row is broken on purpose through the test seam.
TEST(RoutingEngineTest, EachRouteCheckFiresOnABrokenRow) {
  std::vector<FakeStep> steps = repeat(square_step(), 4);
  steps[2].links[1] = down();
  const FakeWorld world(steps, 1.0);
  const auto plan = plan_of(kSquare);
  const auto checks_with = [&](satsim::routing_app::BeforeChecks hook) {
    Engine e = engine({}, plan, 4, kOneWay, 0, {}, std::move(hook));
    (void)run(e, world);
    return e.checks();
  };
  EXPECT_EQ(checks_with({}).total(), 0U);
  // A dropped row reported ok: its route has a link that is down.
  EXPECT_GT(checks_with([](RouteRow& r) {
              if (!r.ok) {
                r.ok = true;
                r.cause.reset();
                r.delay_s = *r.ideal_s;
              }
            }).ok_route_not_in_graph,
            0U);
  EXPECT_GT(checks_with([](RouteRow& r) {
              if (r.ok) {
                r.delay_s = *r.ideal_s * (1.0 - 1e-12);
              }
            }).below_ideal,
            0U);
  EXPECT_GT(checks_with([](RouteRow& r) {
              if (r.ok) {
                r.delay_s = kLowerBound / 2.0;
              }
            }).below_lower_bound,
            0U);
  EXPECT_GT(checks_with([](RouteRow& r) { r.cause.reset(); }).drop_without_cause, 0U);
  EXPECT_GT(checks_with([](RouteRow& r) {
              if (!r.route.empty()) {
                r.route.push_back(r.route.front());
              }
            }).bad_route_shape,
            0U);
}

// The same world twice gives the same rows.
TEST(RoutingEngineTest, Deterministic) {
  const auto plan = plan_of(kRealLinks);
  const FakeWorld world(realistic_steps(12), 1.0);
  const Config s{
      .strategies = {"lowest-delay", "keep-until-broken", "longest-lived", "threshold:1ms"},
      .replan_steps = 3,
      .serving = ServingRule::kLongestInView,
      .backup = true};
  Engine a = realistic_engine(s, plan);
  Engine b = realistic_engine(s, plan);
  const Rows ra = run(a, world);
  const Rows rb = run(b, world);
  for (std::size_t k = 0; k < ra.size(); ++k) {
    for (std::size_t i = 0; i < ra[k].size(); ++i) {
      EXPECT_EQ(ra[k][i].route, rb[k][i].route);
      EXPECT_EQ(ra[k][i].delay_s, rb[k][i].delay_s);
      EXPECT_EQ(ra[k][i].cause, rb[k][i].cause);
      EXPECT_EQ(ra[k][i].backup_s, rb[k][i].backup_s);
    }
  }
}

}  // namespace
