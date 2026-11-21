// The graph of Phase 3 spec 4.2 on a tiny shell worked out by hand: node and edge counts and
// exact delays for both models, the direction of every ground edge, the whole-interval
// intersection, the serving slot's limit and the with-route rule's open dishes.

#include "routing/graph.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "fake.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace {

using satsim::network::Graph;
using satsim::routing_app::EndpointModel;
using satsim::routing_app::GraphBuilder;
using satsim::routing_app::GraphLayout;
using satsim::routing_app::NodeKind;
using satsim::routing_app::NodeMeaning;
using satsim::routing_app::StepGround;
using satsim::routing_app::testing::down;
using satsim::routing_app::testing::plan_of;
using satsim::routing_app::testing::seen;
using satsim::routing_app::testing::up;
namespace topology = satsim::topology;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr double kHop = 0.0001;   // s
constexpr double kFibre = 0.004;  // s, gateway 0 to direction 0's end node
constexpr std::size_t kS = 4;     // satellites
constexpr std::size_t kCities = 2;
constexpr std::size_t kGateways = 1;
constexpr std::size_t kDirections = 2;  // 0: city 0 -> 1 (with a home PoP); 1: 1 -> 0 (none).
constexpr std::size_t kDishes = 2;      // One serving dish per city.

// Satellites 0..3; links 0-1 (1000 km, up), 1-2 (2000 km, up), 2-3 (down). City 0 sees
// satellites 0 (600 km) and 1 (700 km), city 1 sees 2 (800 km); the gateway sees 1 (500 km)
// and 3 (900 km).
struct Tiny {
  std::vector<std::pair<std::size_t, std::size_t>> pairs = {{0, 1}, {1, 2}, {2, 3}};
  topology::LinkPlan plan = plan_of(pairs);
  topology::StepLinks links{.utc = satsim::routing_app::testing::some_time(),
                            .states = {up(1000.0), up(2000.0), down()}};
  std::vector<std::vector<topology::VisibleSatellite>> cities = {
      {seen(0, 50.0, 600.0), seen(1, 40.0, 700.0)}, {seen(2, 45.0, 800.0)}};
  std::vector<std::vector<topology::VisibleSatellite>> gateways = {
      {seen(1, 60.0, 500.0), seen(3, 20.0, 900.0)}};

  [[nodiscard]] StepGround ground() const {
    return {.links = &links, .city_visible = cities, .gateway_visible = gateways};
  }
};

GraphLayout layout(EndpointModel model) {
  return {model, kS, kCities, kGateways, kDirections, kDishes};
}

GraphBuilder builder(const Tiny& t, EndpointModel model) {
  return {layout(model), t.plan, {kFibre, std::nan("")}, {0, 1}, kHop};
}

// The delay of the one edge from -> to; NaN if there is none, and a failure if there are two.
double edge(const Graph& g, std::size_t from, std::size_t to) {
  double delay = std::nan("");
  int found = 0;
  for (const auto& e : g.edges_from(from)) {
    if (e.to == to) {
      delay = e.delay_s;
      ++found;
    }
  }
  EXPECT_LE(found, 1) << from << " -> " << to;
  return delay;
}

// No edge enters a sending dish (open or serving), and none leaves a receiving dish or an end
// node.
void expect_one_way_ends(const Graph& g, const GraphLayout& l) {
  for (std::size_t n = 0; n < g.node_count(); ++n) {
    const NodeKind from = l.meaning(n).kind;
    if (from == NodeKind::kReceivingDish || from == NodeKind::kEnd) {
      EXPECT_TRUE(g.edges_from(n).empty()) << n;
    }
    for (const auto& e : g.edges_from(n)) {
      const NodeKind to = l.meaning(e.to).kind;
      EXPECT_NE(to, NodeKind::kOpenDish) << n << " -> " << e.to;
      EXPECT_NE(to, NodeKind::kServingDish) << n << " -> " << e.to;
    }
  }
}

TEST(RoutingGraphTest, LayoutNumbersEveryNodeOnce) {
  const GraphLayout real = layout(EndpointModel::kRealistic);
  EXPECT_EQ(real.node_count(), kS + 2 * kCities + kGateways + kDirections + kDishes);
  EXPECT_EQ(real.open_dish(1), 6U);
  EXPECT_EQ(real.receiving_dish(1), 7U);
  EXPECT_EQ(real.gateway(0), 8U);
  EXPECT_EQ(real.end(1), 10U);
  EXPECT_EQ(real.serving_dish(1), 12U);
  const std::vector<NodeMeaning> expected = {
      {NodeKind::kSatellite, 0},  {NodeKind::kSatellite, 1},     {NodeKind::kSatellite, 2},
      {NodeKind::kSatellite, 3},  {NodeKind::kOpenDish, 0},      {NodeKind::kReceivingDish, 0},
      {NodeKind::kOpenDish, 1},   {NodeKind::kReceivingDish, 1}, {NodeKind::kGateway, 0},
      {NodeKind::kEnd, 0},        {NodeKind::kEnd, 1},           {NodeKind::kServingDish, 0},
      {NodeKind::kServingDish, 1}};
  for (std::size_t n = 0; n < expected.size(); ++n) {
    EXPECT_EQ(real.meaning(n), expected[n]) << n;
  }
  // The best case has no gateway, end or serving node: the topology program's numbering.
  const GraphLayout best = layout(EndpointModel::kBestCase);
  EXPECT_EQ(best.node_count(), kS + 2 * kCities);
  EXPECT_EQ(best.gateway_count() + best.direction_count() + best.serving_dish_count(), 0U);
}

// Best case: 2 up links (4 directed edges), city 0's 2 satellites and city 1's 1, each both
// ways (dish -> satellite and satellite -> receiving dish): 4 + 4 + 2 = 10 edges.
TEST(RoutingGraphTest, BestCaseCountsAndDelays) {
  const Tiny t;
  const GraphBuilder b = builder(t, EndpointModel::kBestCase);
  const GraphLayout& l = b.layout();
  Graph g(l.node_count());
  const StepGround step = t.ground();
  ASSERT_FALSE(b.build({&step, 1}, {}, 1, false, g).has_value());
  EXPECT_EQ(g.node_count(), 8U);
  EXPECT_EQ(g.edge_count(), 10U);
  EXPECT_EQ(edge(g, 0, 1), 1000.0 / kC + kHop);
  EXPECT_EQ(edge(g, 1, 0), 1000.0 / kC + kHop);
  EXPECT_EQ(edge(g, 2, 1), 2000.0 / kC + kHop);
  EXPECT_TRUE(std::isnan(edge(g, 2, 3)));  // Down.
  EXPECT_EQ(edge(g, l.open_dish(0), 0), 600.0 / kC + kHop);
  EXPECT_EQ(edge(g, l.open_dish(0), 1), 700.0 / kC + kHop);
  EXPECT_EQ(edge(g, 0, l.receiving_dish(0)), 600.0 / kC + kHop);
  EXPECT_EQ(edge(g, 2, l.receiving_dish(1)), 800.0 / kC + kHop);
  expect_one_way_ends(g, l);
}

// Realistic: 4 laser edges; open dishes 2 + 1; the gateway's 2 satellites; one end edge
// (direction 1's sender has no home PoP); serving dish 0 to its satellite 1 (dish 1's
// satellite 3 is not seen by city 1: no edge): 4 + 3 + 2 + 1 + 1 = 11 edges.
TEST(RoutingGraphTest, RealisticCountsAndDelays) {
  const Tiny t;
  const GraphBuilder b = builder(t, EndpointModel::kRealistic);
  const GraphLayout& l = b.layout();
  Graph g(l.node_count());
  const StepGround step = t.ground();
  const std::vector<std::optional<std::size_t>> serving = {1, 3};
  ASSERT_FALSE(b.build({&step, 1}, serving, 1, false, g).has_value());
  EXPECT_EQ(g.node_count(), 13U);
  EXPECT_EQ(g.edge_count(), 11U);
  EXPECT_EQ(edge(g, l.serving_dish(0), 1), 700.0 / kC + kHop);
  EXPECT_TRUE(g.edges_from(l.serving_dish(1)).empty());
  EXPECT_EQ(edge(g, 1, l.gateway(0)), 500.0 / kC + kHop);
  EXPECT_EQ(edge(g, 3, l.gateway(0)), 900.0 / kC + kHop);
  EXPECT_EQ(edge(g, l.gateway(0), l.end(0)), kFibre);  // Fibre: no hop delay.
  EXPECT_TRUE(std::isnan(edge(g, l.gateway(0), l.end(1))));
  EXPECT_TRUE(g.edges_from(l.receiving_dish(0)).empty());  // No receiving dish in this model.
  EXPECT_TRUE(std::isnan(edge(g, 2, l.receiving_dish(1))));
  expect_one_way_ends(g, l);
}

// The whole interval keeps only what every step has: at the second step link 0-1 is down and
// city 0 no longer sees satellite 0. The serving edge is tested only over the rest of the slot.
TEST(RoutingGraphTest, WholeIntervalKeepsEdgesPresentAtEveryStep) {
  const Tiny first;
  Tiny second;
  second.links.states[0] = down();
  second.cities[0] = {seen(1, 41.0, 710.0)};
  const std::vector<StepGround> steps = {first.ground(), second.ground()};
  const GraphBuilder b = builder(first, EndpointModel::kRealistic);
  const GraphLayout& l = b.layout();
  Graph g(l.node_count());
  const std::vector<std::optional<std::size_t>> serving = {0, std::nullopt};

  ASSERT_FALSE(b.build(steps, serving, 2, false, g).has_value());
  EXPECT_TRUE(std::isnan(edge(g, 0, 1)));
  EXPECT_EQ(edge(g, 1, 2), 2000.0 / kC + kHop);
  EXPECT_TRUE(std::isnan(edge(g, l.open_dish(0), 0)));
  EXPECT_EQ(edge(g, l.open_dish(0), 1), 700.0 / kC + kHop);  // The first step's delay.
  EXPECT_TRUE(g.edges_from(l.serving_dish(0)).empty());      // Satellite 0 leaves in the slot.

  // The slot ends after the first step: the next slot's satellite is not predicted.
  ASSERT_FALSE(b.build(steps, serving, 1, false, g).has_value());
  EXPECT_EQ(edge(g, l.serving_dish(0), 0), 600.0 / kC + kHop);
}

// With-route: every serving dish is open, with an edge to every satellite its city sees.
TEST(RoutingGraphTest, OpenServingDishesSeeEverySatelliteOfTheirCity) {
  const Tiny t;
  const GraphBuilder b = builder(t, EndpointModel::kRealistic);
  const GraphLayout& l = b.layout();
  Graph g(l.node_count());
  const StepGround step = t.ground();
  const std::vector<std::optional<std::size_t>> serving(kDishes);
  ASSERT_FALSE(b.build({&step, 1}, serving, 1, true, g).has_value());
  EXPECT_EQ(g.edges_from(l.serving_dish(0)).size(), 2U);
  EXPECT_EQ(edge(g, l.serving_dish(1), 2), 800.0 / kC + kHop);
  expect_one_way_ends(g, l);
}

TEST(RoutingGraphTest, MismatchedInputsAreRefused) {
  Tiny t;
  const GraphBuilder b = builder(t, EndpointModel::kRealistic);
  Graph wrong(3);
  const StepGround step = t.ground();
  const std::vector<std::optional<std::size_t>> serving(kDishes);
  EXPECT_TRUE(b.build({&step, 1}, serving, 1, false, wrong).has_value());
  Graph g(b.layout().node_count());
  EXPECT_TRUE(b.build({}, serving, 1, false, g).has_value());
  t.links.states.pop_back();
  const StepGround short_links = t.ground();
  EXPECT_TRUE(b.build({&short_links, 1}, serving, 1, false, g).has_value());
}

}  // namespace
