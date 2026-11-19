// The two endpoint models on small constructed worlds whose delays are worked by hand: a
// chain of five satellites with chosen laser lengths, cities and gateways with chosen
// slant ranges. Geometry is not recomputed here (the topology module's tests cover it): the
// models only read link lengths and slant ranges.

#include "topology/endpoints.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/distance.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/utc_time.hpp"
#include "satsim/topology/ground_links.hpp"
#include "satsim/topology/link_plan.hpp"
#include "satsim/topology/link_state.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::ground::GroundSite;
using satsim::ground::make_ground_site;
using satsim::topology::VisibleSatellite;
using satsim::topology_app::Direction;
using satsim::topology_app::EndpointEvaluator;
using satsim::topology_app::EndpointModel;
using satsim::topology_app::EndpointSettings;
using satsim::topology_app::GatewaySite;
using satsim::topology_app::GatewaySource;
using satsim::topology_app::PathResult;
using satsim::topology_app::PathStatus;
using Visible = std::vector<std::vector<VisibleSatellite>>;

constexpr double kC = satsim::kSpeedOfLightVacuumKmPerS;
constexpr std::size_t kSatellites = 5;

// Sums of a few delays come out in different orders here and in the evaluator.
constexpr double kTolerance_s = 1e-15;

// The chain 0 -1000- 1 -2000- 2 -1500- 3 -1000- 4 (km); `down` lists links that are down.
struct Chain {
  satsim::topology::LinkPlan plan;
  satsim::topology::StepLinks links;
};

Chain chain(const std::vector<std::size_t>& down = {}) {
  const std::vector<double> lengths_km = {1000.0, 2000.0, 1500.0, 1000.0};
  Chain c{.plan = {},
          .links = {.utc = satsim::constellation::testing::utc({2026, 11, 18, 12, 0, 0.0}),
                    .states = {}}};
  for (std::size_t i = 0; i < lengths_km.size(); ++i) {
    c.plan.links.push_back({.satellite_a = i, .satellite_b = i + 1});
    satsim::topology::LinkState state{.length_km = lengths_km[i], .down_reason = std::nullopt};
    for (const std::size_t d : down) {
      if (d == i) {
        state.down_reason = satsim::topology::LinkDownReason::kNoLineOfSight;
      }
    }
    c.links.states.push_back(state);
  }
  return c;
}

VisibleSatellite sees(std::size_t satellite, double elevation_deg, double slant_km) {
  return {.satellite = satellite,
          .norad_cat_id = 1000 + static_cast<std::int64_t>(satellite),
          .shell_index = 0,
          .elevation_rad = elevation_deg * std::numbers::pi / 180.0,
          .slant_range_km = slant_km,
          .mask_rad = 0.0};
}

// Cities 0 Alpha, 1 Beta, 2 Gamma; gateways 0 near Alpha, 1 near Beta.
const GroundSite kAlpha = make_ground_site("Alpha", 40.0, -75.0);
const GroundSite kBeta = make_ground_site("Beta", 42.0, -88.0);
const GroundSite kGamma = make_ground_site("Gamma", 48.8, 2.3);
const GroundSite kGatewayA = make_ground_site("Gateway A", 40.5, -74.5);
const GroundSite kGatewayB = make_ground_site("Gateway B", 41.5, -87.5);
const GroundSite kPopB = make_ground_site("PoP B", 42.1, -88.1);

std::vector<GatewaySite> two_gateways() {
  return {{.site = kGatewayA, .source = GatewaySource::kReal},
          {.site = kGatewayB, .source = GatewaySource::kAssumed}};
}

double gc_km(const GroundSite& a, const GroundSite& b) {
  return satsim::ground::great_circle_distance_on_mean_sphere_km(a, b);
}

double fibre_s(const GroundSite& a, const GroundSite& b, double factor = 1.98) {
  return satsim::ground::fibre_delay_s(gc_km(a, b), factor);
}

struct World {
  Chain net = chain();
  std::vector<std::optional<GroundSite>> home_pops = {kGatewayA, kPopB, std::nullopt};
  std::vector<GatewaySite> gateways = two_gateways();
  std::vector<Direction> directions = {{.from = 0, .to = 1}};
  EndpointSettings settings{};
  Visible cities = {{sees(0, 70.0, 500.0), sees(1, 30.0, 900.0)}, {sees(4, 60.0, 700.0)}, {}};
  Visible gateway_visible = {{sees(0, 65.0, 520.0)}, {sees(3, 50.0, 600.0)}};

  std::vector<PathResult> run() {
    EndpointEvaluator evaluator({kAlpha, kBeta, kGamma}, home_pops, gateways, directions,
                                kSatellites, settings);
    std::vector<PathResult> results;
    const auto error = evaluator.evaluate(net.plan, net.links, cities, gateway_visible, results);
    EXPECT_FALSE(error.has_value()) << error.value_or("");
    return results;
  }
};

TEST(EndpointsTest, Names) {
  using satsim::topology_app::name_of;
  EXPECT_EQ(name_of(EndpointModel::kRealistic), "realistic");
  EXPECT_EQ(name_of(EndpointModel::kBestCase), "best-case");
  EXPECT_EQ(satsim::topology_app::parse_endpoint_model("best-case"), EndpointModel::kBestCase);
  EXPECT_EQ(satsim::topology_app::parse_endpoint_model("realistic"), EndpointModel::kRealistic);
  EXPECT_FALSE(satsim::topology_app::parse_endpoint_model("best_case").has_value());
  EXPECT_EQ(name_of(PathStatus::kOk), "ok");
  EXPECT_EQ(name_of(PathStatus::kNoService), "no_service");
  EXPECT_EQ(name_of(PathStatus::kNoCoverage), "no_coverage");
  EXPECT_EQ(name_of(PathStatus::kNoGateway), "no_gateway");
  EXPECT_EQ(name_of(PathStatus::kNoPath), "no_path");
  EXPECT_EQ(name_of(GatewaySource::kReal), "real");
  EXPECT_EQ(name_of(GatewaySource::kAssumed), "assumed");
}

// Alpha's serving satellite (0, the highest) is seen by gateway A, where Alpha's home PoP is:
// up 500 km, down 520 km, no laser, no fibre to the PoP, then fibre PoP (= gateway A) to Beta.
// Gateway B's route (four laser hops and fibre back to the PoP) is far longer.
TEST(EndpointsTest, RealisticBentPipe) {
  World w;
  const PathResult r = w.run().at(0);
  ASSERT_EQ(r.status, PathStatus::kOk);
  EXPECT_NEAR(r.one_way_s, (500.0 + 520.0) / kC + fibre_s(kGatewayA, kBeta), kTolerance_s);
  EXPECT_EQ(r.laser_hops, 0U);
  EXPECT_EQ(r.uplink_km, 500.0);
  EXPECT_EQ(r.laser_km, 0.0);
  EXPECT_EQ(r.downlink_km, 520.0);
  EXPECT_DOUBLE_EQ(r.fibre_km, gc_km(kGatewayA, kBeta));
  EXPECT_DOUBLE_EQ(r.path_km(), 500.0 + 520.0 + gc_km(kGatewayA, kBeta));
  EXPECT_EQ(r.serving_satellite, 0U);
  EXPECT_EQ(r.gateway_satellite, 0U);
  EXPECT_EQ(r.gateway, 0U);
  EXPECT_FALSE(r.first_satellite.has_value());
  EXPECT_FALSE(r.last_satellite.has_value());
}

// Gateway A sees nothing: from satellite 0 over lasers 0-1-2-3 (4500 km, three hops) down to
// gateway B (600 km); Alpha's PoP is put at gateway B, so the only fibre is to Beta.
TEST(EndpointsTest, RealisticLaserRoute) {
  World w;
  w.gateway_visible[0].clear();
  w.home_pops[0] = kGatewayB;
  const PathResult r = w.run().at(0);
  ASSERT_EQ(r.status, PathStatus::kOk);
  EXPECT_NEAR(r.one_way_s, (500.0 + 4500.0 + 600.0) / kC + fibre_s(kGatewayB, kBeta), kTolerance_s);
  EXPECT_EQ(r.laser_hops, 3U);
  EXPECT_NEAR(r.laser_km, 4500.0, 1e-9);
  EXPECT_EQ(r.downlink_km, 600.0);
  EXPECT_EQ(r.gateway, 1U);
  EXPECT_EQ(r.gateway_satellite, 3U);
  EXPECT_EQ(r.serving_satellite, 0U);
}

// Both gateways reachable, Alpha's PoP at gateway B. Through gateway A: 500 + 520 km and fibre
// gateway A to B (about 1,100 km). Through gateway B: 500 + 4500 + 600 km and no fibre. With
// the default factor 1.98 gateway A wins; with factor 5 the fibre costs more than the lasers.
TEST(EndpointsTest, RealisticChoiceWeighsFibre) {
  for (const double factor : {1.98, 5.0}) {
    World w;
    w.home_pops[0] = kGatewayB;
    w.settings.fibre_factor = factor;
    const PathResult r = w.run().at(0);
    ASSERT_EQ(r.status, PathStatus::kOk);
    const double via_a = (500.0 + 520.0) / kC + fibre_s(kGatewayA, kGatewayB, factor) +
                         fibre_s(kGatewayB, kBeta, factor);
    const double via_b = (500.0 + 4500.0 + 600.0) / kC + fibre_s(kGatewayB, kBeta, factor);
    if (factor < 2.0) {
      ASSERT_LT(via_a, via_b);
      EXPECT_EQ(r.gateway, 0U);
      EXPECT_NEAR(r.one_way_s, via_a, kTolerance_s);
      EXPECT_DOUBLE_EQ(r.fibre_km, gc_km(kGatewayA, kGatewayB) + gc_km(kGatewayB, kBeta));
    } else {
      ASSERT_LT(via_b, via_a);
      EXPECT_EQ(r.gateway, 1U);
      EXPECT_NEAR(r.one_way_s, via_b, kTolerance_s);
    }
  }
}

// Two gateways at the same place seeing the same satellite at the same range: an exact tie,
// broken by the lower gateway index.
TEST(EndpointsTest, RealisticTiesGoToTheLowerGateway) {
  World w;
  w.gateways = {{.site = kGatewayA, .source = GatewaySource::kReal},
                {.site = kGatewayA, .source = GatewaySource::kAssumed}};
  w.gateway_visible = {{sees(0, 65.0, 520.0)}, {sees(0, 65.0, 520.0)}};
  EXPECT_EQ(w.run().at(0).gateway, 0U);
  // Swapped: still the lower index, whichever source it has.
  w.gateways = {{.site = kGatewayA, .source = GatewaySource::kAssumed},
                {.site = kGatewayA, .source = GatewaySource::kReal}};
  EXPECT_EQ(w.run().at(0).gateway, 0U);
}

TEST(EndpointsTest, RealisticNoServiceNoCoverageNoGateway) {
  {
    // Gamma has no home PoP: no service, whatever it sees.
    World w;
    w.directions = {{.from = 2, .to = 0}};
    w.cities[2] = {sees(0, 80.0, 480.0)};
    const PathResult r = w.run().at(0);
    EXPECT_EQ(r.status, PathStatus::kNoService);
    EXPECT_FALSE(r.serving_satellite.has_value());
    EXPECT_EQ(r.one_way_s, 0.0);
  }
  {
    // Alpha sees nothing.
    World w;
    w.cities[0].clear();
    const PathResult r = w.run().at(0);
    EXPECT_EQ(r.status, PathStatus::kNoCoverage);
    EXPECT_FALSE(r.serving_satellite.has_value());
  }
  {
    // Link 0-1 down: satellite 0 is cut off; gateway A sees nothing; gateway B sees 3.
    World w;
    w.net = chain({0});
    w.gateway_visible[0].clear();
    const PathResult r = w.run().at(0);
    EXPECT_EQ(r.status, PathStatus::kNoGateway);
    EXPECT_EQ(r.serving_satellite, 0U);
    EXPECT_FALSE(r.gateway.has_value());
    EXPECT_FALSE(r.gateway_satellite.has_value());
    EXPECT_EQ(r.path_km(), 0.0);
  }
  {
    // No gateways at all.
    World w;
    w.gateways.clear();
    w.gateway_visible.clear();
    EXPECT_EQ(w.run().at(0).status, PathStatus::kNoGateway);
  }
}

// Best case Alpha to Beta: from satellite 0 (500 km up) the lasers to 4 are 5500 km; from
// satellite 1 (900 km up) 4500 km. Via 1: 900 + 4500 + 700 = 6100 km, three hops.
TEST(EndpointsTest, BestCaseUsesAnyVisibleSatellite) {
  World w;
  w.settings.model = EndpointModel::kBestCase;
  w.directions = {{.from = 0, .to = 1}, {.from = 1, .to = 0}};
  const std::vector<PathResult> results = w.run();
  const PathResult& r = results.at(0);
  ASSERT_EQ(r.status, PathStatus::kOk);
  EXPECT_NEAR(r.one_way_s, 6100.0 / kC, kTolerance_s);
  EXPECT_EQ(r.laser_hops, 3U);
  EXPECT_EQ(r.uplink_km, 900.0);
  EXPECT_NEAR(r.laser_km, 4500.0, 1e-9);
  EXPECT_EQ(r.downlink_km, 700.0);
  EXPECT_EQ(r.fibre_km, 0.0);
  EXPECT_EQ(r.first_satellite, 1U);
  EXPECT_EQ(r.last_satellite, 4U);
  EXPECT_FALSE(r.serving_satellite.has_value());
  EXPECT_FALSE(r.gateway.has_value());
  // The reverse direction: the same path backwards.
  const PathResult& back = results.at(1);
  ASSERT_EQ(back.status, PathStatus::kOk);
  EXPECT_NEAR(back.one_way_s, 6100.0 / kC, kTolerance_s);
  EXPECT_EQ(back.first_satellite, 4U);
  EXPECT_EQ(back.last_satellite, 1U);
  EXPECT_EQ(back.uplink_km, 700.0);
  EXPECT_EQ(back.downlink_km, 900.0);
}

// A city that is the shortest way across is never used as a relay: Gamma sees satellites on
// both sides of a broken link, but best case stays in space.
TEST(EndpointsTest, BestCaseNoPathAndNoRelayThroughACity) {
  World w;
  w.settings.model = EndpointModel::kBestCase;
  w.net = chain({2});
  w.cities[2] = {sees(1, 50.0, 800.0), sees(4, 50.0, 800.0)};
  EXPECT_EQ(w.run().at(0).status, PathStatus::kNoPath);

  w.cities[1].clear();
  EXPECT_EQ(w.run().at(0).status, PathStatus::kNoCoverage);
  World v;
  v.settings.model = EndpointModel::kBestCase;
  v.cities[0].clear();
  EXPECT_EQ(v.run().at(0).status, PathStatus::kNoCoverage);
}

// The serving-satellite restriction: a gateway at Beta itself, seeing Beta's satellite, and
// Alpha's PoP at Beta, so the realistic path has no fibre and the same satellites as the best
// case; realistic must use satellite 0 (highest) and so is longer than the best case, which
// uses satellite 1. 500 + 5500 + 700 = 6700 km against 6100 km.
TEST(EndpointsTest, RealisticIsNeverShorterThanBestCaseOnTheSameSatellites) {
  World w;
  w.gateways = {{.site = kBeta, .source = GatewaySource::kReal}};
  w.gateway_visible = {{sees(4, 60.0, 700.0)}};
  w.home_pops[0] = kBeta;
  const PathResult realistic = w.run().at(0);
  w.settings.model = EndpointModel::kBestCase;
  const PathResult best = w.run().at(0);
  ASSERT_EQ(realistic.status, PathStatus::kOk);
  ASSERT_EQ(best.status, PathStatus::kOk);
  EXPECT_NEAR(realistic.one_way_s, 6700.0 / kC, kTolerance_s);
  EXPECT_NEAR(best.one_way_s, 6100.0 / kC, kTolerance_s);
  EXPECT_GT(realistic.one_way_s, best.one_way_s);
  EXPECT_EQ(realistic.serving_satellite, 0U);
  EXPECT_EQ(best.first_satellite, 1U);
}

// Alpha to Beta goes through Alpha's PoP (at gateway A); Beta to Alpha through Beta's PoP,
// from Beta's serving satellite 4 over lasers to whichever gateway costs least.
TEST(EndpointsTest, RealisticDirectionsDifferWhenHomePopsDiffer) {
  World w;
  w.directions = {{.from = 0, .to = 1}, {.from = 1, .to = 0}};
  const std::vector<PathResult> results = w.run();
  const PathResult& ab = results.at(0);
  const PathResult& ba = results.at(1);
  ASSERT_EQ(ab.status, PathStatus::kOk);
  ASSERT_EQ(ba.status, PathStatus::kOk);
  EXPECT_NEAR(ab.one_way_s, (500.0 + 520.0) / kC + fibre_s(kGatewayA, kBeta), kTolerance_s);
  // Beta: up 700 to satellite 4. Gateway B through 3 (1000 km of laser, 600 down) then fibre
  // gateway B to PoP B; gateway A through 0 (5500 km, 520 down) then fibre gateway A to PoP B.
  const double via_b = (700.0 + 1000.0 + 600.0) / kC + fibre_s(kGatewayB, kPopB);
  const double via_a = (700.0 + 5500.0 + 520.0) / kC + fibre_s(kGatewayA, kPopB);
  ASSERT_LT(via_b, via_a);
  EXPECT_NEAR(ba.one_way_s, via_b + fibre_s(kPopB, kAlpha), kTolerance_s);
  EXPECT_EQ(ba.serving_satellite, 4U);
  EXPECT_EQ(ba.gateway, 1U);
  EXPECT_EQ(ba.laser_hops, 1U);
  EXPECT_NE(ab.one_way_s, ba.one_way_s);
}

TEST(EndpointsTest, BadGraphInputsAreErrors) {
  World w;
  EndpointEvaluator evaluator({kAlpha, kBeta, kGamma}, w.home_pops, w.gateways, w.directions,
                              kSatellites, w.settings);
  std::vector<PathResult> results;
  // A negative slant range.
  Visible cities = w.cities;
  cities[0][0].slant_range_km = -1.0;
  EXPECT_TRUE(
      evaluator.evaluate(w.net.plan, w.net.links, cities, w.gateway_visible, results).has_value());
  // Link states that do not match the plan.
  Chain short_links = chain();
  short_links.links.states.pop_back();
  EXPECT_TRUE(
      evaluator.evaluate(short_links.plan, short_links.links, w.cities, w.gateway_visible, results)
          .has_value());
  // Gateway lists that do not match the gateways.
  const Visible one_gateway = {w.gateway_visible[0]};
  EXPECT_TRUE(
      evaluator.evaluate(w.net.plan, w.net.links, w.cities, one_gateway, results).has_value());
  // And the good inputs still work afterwards.
  EXPECT_FALSE(evaluator.evaluate(w.net.plan, w.net.links, w.cities, w.gateway_visible, results)
                   .has_value());
  EXPECT_EQ(results.at(0).status, PathStatus::kOk);
}

}  // namespace
