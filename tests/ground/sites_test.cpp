#include "satsim/ground/sites.hpp"

#include <gtest/gtest.h>

#include <numbers>
#include <vector>

#include "satsim/frames/geodetic.hpp"
#include "satsim/frames/wgs84.hpp"

namespace {

using satsim::frames::kWgs84EquatorialRadiusKm;
using satsim::frames::kWgs84PolarRadiusKm;
using satsim::ground::Gateway;
using satsim::ground::GatewaySelection;
using satsim::ground::GatewayStatus;
using satsim::ground::make_ground_site;
using satsim::ground::select_gateways;

constexpr double kKmTolerance = 1e-9;

TEST(GroundSiteTest, StoresRadiansAndTheEllipsoidPosition) {
  const auto site = make_ground_site("Origin", 0.0, 0.0);
  EXPECT_EQ(site.name, "Origin");
  EXPECT_EQ(site.latitude_rad, 0.0);
  EXPECT_EQ(site.longitude_rad, 0.0);
  EXPECT_NEAR(site.position_ecef_km.x(), kWgs84EquatorialRadiusKm, kKmTolerance);
  EXPECT_NEAR(site.position_ecef_km.y(), 0.0, kKmTolerance);
  EXPECT_NEAR(site.position_ecef_km.z(), 0.0, kKmTolerance);

  const auto east = make_ground_site("East", 0.0, 90.0);
  EXPECT_NEAR(east.longitude_rad, std::numbers::pi / 2.0, 1e-15);
  EXPECT_NEAR(east.position_ecef_km.y(), kWgs84EquatorialRadiusKm, kKmTolerance);

  const auto pole = make_ground_site("North pole", 90.0, 0.0);
  EXPECT_NEAR(pole.latitude_rad, std::numbers::pi / 2.0, 1e-15);
  EXPECT_NEAR(pole.position_ecef_km.norm(), kWgs84PolarRadiusKm, kKmTolerance);
  EXPECT_NEAR(pole.position_ecef_km.z(), kWgs84PolarRadiusKm, kKmTolerance);
}

TEST(GroundSiteTest, PositionIsOnTheEllipsoidAtHeightZero) {
  const auto london = make_ground_site("London", 51.5074, -0.1278);
  const auto geodetic = satsim::frames::ecef_to_geodetic(london.position_ecef_km);
  EXPECT_NEAR(geodetic.latitude_rad, london.latitude_rad, 1e-12);
  EXPECT_NEAR(geodetic.longitude_rad, london.longitude_rad, 1e-12);
  EXPECT_NEAR(geodetic.height_km, 0.0, 1e-9);
}

TEST(SelectGatewaysTest, DefaultsToOperationalOnlyInInputOrder) {
  std::vector<Gateway> gateways(4);
  gateways[0].site.name = "a";
  gateways[0].status = GatewayStatus::kPlanned;
  gateways[1].site.name = "b";
  gateways[1].status = GatewayStatus::kOperational;
  gateways[2].site.name = "c";
  gateways[2].status = GatewayStatus::kPlanned;
  gateways[3].site.name = "d";
  gateways[3].status = GatewayStatus::kOperational;

  const auto operational = select_gateways(gateways);
  ASSERT_EQ(operational.size(), 2U);
  EXPECT_EQ(operational[0].site.name, "b");
  EXPECT_EQ(operational[1].site.name, "d");
  EXPECT_EQ(select_gateways(gateways, GatewaySelection::kOperationalOnly).size(), 2U);

  const auto all = select_gateways(gateways, GatewaySelection::kOperationalAndPlanned);
  ASSERT_EQ(all.size(), 4U);
  EXPECT_EQ(all[0].site.name, "a");
  EXPECT_EQ(all[3].site.name, "d");

  EXPECT_TRUE(select_gateways({}).empty());
}

}  // namespace
