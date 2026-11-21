#include "failures/control_graph.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

#include "fake_geometry.hpp"
#include "inputs/inputs.hpp"
#include "routing/graph.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/network/graph.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"

namespace {

using satsim::app::EndpointModel;
using satsim::failures_app::ControlNodes;
using satsim::failures_app::testing::seen;
using satsim::network::Edge;
using satsim::topology::LinkDownReason;
using satsim::topology::LinkState;
using satsim::topology::PlannedLink;
using satsim::topology::StepLinks;
using satsim::topology::VisibleSatellite;

constexpr double kHop = 0.002;

double delay(double km) { return km / satsim::kSpeedOfLightVacuumKmPerS + kHop; }

// 4 satellites, 2 cities, 2 gateways: nodes 0-3, dishes 4 and 5, gateways 6 and 7.
// Links: 0-1 up (1000 km), 1-2 down, 0-3 up (1200 km) but satellite 3 is down.
// City 0 sees 1 (500 km) and 3; city 1 sees 2 (520 km). Gateway 0 sees 0 (600 km) and 3;
// gateway 1 (down) sees 2.
struct Fixture {
  ControlNodes nodes{.satellites = 4, .cities = 2, .gateways = 2};
  std::vector<PlannedLink> links = {PlannedLink{.satellite_a = 0, .satellite_b = 1},
                                    PlannedLink{.satellite_a = 1, .satellite_b = 2},
                                    PlannedLink{.satellite_a = 0, .satellite_b = 3}};
  StepLinks states{.utc = satsim::failures_app::testing::some_time(), .states = {}};
  std::vector<std::vector<VisibleSatellite>> cities = {{seen(1, 500.0), seen(3, 530.0)},
                                                       {seen(2, 520.0)}};
  std::vector<std::vector<VisibleSatellite>> gateways = {{seen(0, 600.0), seen(3, 640.0)},
                                                         {seen(2, 650.0)}};
  std::vector<char> satellite_down = {0, 0, 0, 1};
  std::vector<char> gateway_down = {0, 1};

  Fixture() {
    states.states = {LinkState{.length_km = 1000.0, .down_reason = std::nullopt},
                     LinkState{.length_km = 1100.0, .down_reason = LinkDownReason::kFailed},
                     LinkState{.length_km = 1200.0, .down_reason = std::nullopt}};
  }

  [[nodiscard]] satsim::routing_app::StepGround ground() const {
    return {.links = &states,
            .city_visible = cities,
            .gateway_visible = gateways,
            .satellite_down = satellite_down,
            .gateway_down = gateway_down};
  }
};

std::vector<Edge> edges(const satsim::network::Graph& g, std::size_t node) {
  const auto span = g.edges_from(node);
  return {span.begin(), span.end()};
}

TEST(ControlGraphTest, NodeNumbering) {
  const ControlNodes n{.satellites = 4, .cities = 2, .gateways = 2};
  EXPECT_EQ(n.node_count(), 8U);
  EXPECT_EQ(n.dish(1), 5U);
  EXPECT_EQ(n.gateway(0), 6U);
  EXPECT_TRUE(n.is_satellite(3));
  EXPECT_TRUE(n.is_dish(4));
  EXPECT_FALSE(n.is_dish(6));
  EXPECT_TRUE(n.is_gateway(7));
  EXPECT_FALSE(n.is_gateway(8));
}

TEST(ControlGraphTest, BestCase) {
  const Fixture f;
  const satsim::network::Graph g = satsim::failures_app::control_graph(
      f.nodes, f.links, f.ground(), EndpointModel::kBestCase, kHop);
  ASSERT_EQ(g.node_count(), 8U);
  EXPECT_EQ(edges(g, 0), (std::vector<Edge>{{1, delay(1000.0)}, {6, delay(600.0)}}));
  EXPECT_EQ(edges(g, 1), (std::vector<Edge>{{0, delay(1000.0)}, {4, delay(500.0)}}));
  EXPECT_EQ(edges(g, 2), (std::vector<Edge>{{5, delay(520.0)}}));
  EXPECT_TRUE(edges(g, 3).empty());
  EXPECT_EQ(edges(g, 4), (std::vector<Edge>{{1, delay(500.0)}}));
  EXPECT_EQ(edges(g, 5), (std::vector<Edge>{{2, delay(520.0)}}));
  EXPECT_EQ(edges(g, 6), (std::vector<Edge>{{0, delay(600.0)}}));
  EXPECT_TRUE(edges(g, 7).empty());
  EXPECT_EQ(g.edge_count(), 8U);
}

TEST(ControlGraphTest, RealisticHasNoDishEdges) {
  const Fixture f;
  const satsim::network::Graph g = satsim::failures_app::control_graph(
      f.nodes, f.links, f.ground(), EndpointModel::kRealistic, kHop);
  EXPECT_EQ(edges(g, 0), (std::vector<Edge>{{1, delay(1000.0)}, {6, delay(600.0)}}));
  EXPECT_EQ(edges(g, 1), (std::vector<Edge>{{0, delay(1000.0)}}));
  EXPECT_TRUE(edges(g, 2).empty());
  EXPECT_TRUE(edges(g, 4).empty());
  EXPECT_TRUE(edges(g, 5).empty());
  EXPECT_EQ(g.edge_count(), 4U);
}

}  // namespace
