#include "satsim/topology/geometry.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/frames/wgs84.hpp"
#include "satsim/ground/sites.hpp"

namespace {

using satsim::ground::GroundSite;
using satsim::ground::make_ground_site;
using satsim::topology::elevation_mask_rad;
using satsim::topology::ElevationMaskSettings;
using satsim::topology::has_line_of_sight;
using satsim::topology::line_of_sight_radius_km;
using satsim::topology::look_angle;
using satsim::topology::LookAngle;
using satsim::topology::max_link_length_km;
using satsim::topology::segment_min_radius_km;
using satsim::topology::site_geometry;
using satsim::topology::SiteGeometry;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;

TEST(LineOfSightTest, RadiusIsEquatorialRadiusPlusLayer) {
  EXPECT_DOUBLE_EQ(line_of_sight_radius_km(), 6378.137 + 80.0);
  EXPECT_DOUBLE_EQ(line_of_sight_radius_km(0.0), satsim::frames::kWgs84EquatorialRadiusKm);
}

// A segment whose closest point to the centre is its midpoint, at exactly the limit, just
// above and just below.
TEST(LineOfSightTest, AtAboveAndBelowTheLimit) {
  const double r = line_of_sight_radius_km();
  const double half_km = 2000.0;
  const Eigen::Vector3d a(r, -half_km, 0.0);
  const Eigen::Vector3d b(r, half_km, 0.0);
  EXPECT_EQ(segment_min_radius_km(a, b), r);
  EXPECT_TRUE(has_line_of_sight(a, b));

  const Eigen::Vector3d up(1e-6, 0.0, 0.0);
  EXPECT_TRUE(has_line_of_sight(a + up, b + up));
  EXPECT_FALSE(has_line_of_sight(a - up, b - up));

  // Another layer height moves the limit with it.
  EXPECT_FALSE(has_line_of_sight(a, b, 80.001));
  EXPECT_TRUE(has_line_of_sight(a, b, 79.999));
}

// When the perpendicular from the centre falls outside the segment, the nearer endpoint is
// the closest point: here a satellite and one farther out beyond it.
TEST(LineOfSightTest, MinimumAtAnEndpoint) {
  const Eigen::Vector3d a(7000.0, 0.0, 0.0);
  const Eigen::Vector3d b(8000.0, 100.0, 0.0);
  EXPECT_EQ(segment_min_radius_km(a, b), 7000.0);
  EXPECT_EQ(segment_min_radius_km(b, a), 7000.0);
  // The infinite line comes much closer (about 87 km from the centre), the segment does not.
  EXPECT_TRUE(has_line_of_sight(a, b));
  // Both ends below the layer: no line of sight even if the segment is short.
  const Eigen::Vector3d low(6400.0, 0.0, 0.0);
  EXPECT_FALSE(has_line_of_sight(low, low + Eigen::Vector3d(0.0, 1.0, 0.0)));
  // A degenerate segment is its point.
  EXPECT_EQ(segment_min_radius_km(a, a), 7000.0);
}

TEST(LineOfSightTest, AntipodalSatellitesHaveNone) {
  const Eigen::Vector3d a(0.0, 0.0, 6900.0);
  const Eigen::Vector3d b(0.0, 0.0, -6900.0);
  EXPECT_EQ(segment_min_radius_km(a, b), 0.0);
  EXPECT_FALSE(has_line_of_sight(a, b));
  EXPECT_FALSE(
      has_line_of_sight(Eigen::Vector3d(6900.0, 0.0, 0.0), Eigen::Vector3d(-6900.0, 1.0, 0.0)));
}

// The longest link at 462 km: about 4,508 km (spec 4.3 says "about 4,500"). A segment of
// exactly that length tangent to the limiting sphere at its midpoint has line of sight.
TEST(LineOfSightTest, LongestLinkBetweenTwoRadii) {
  const double r = line_of_sight_radius_km();
  const double radius = 6378.137 + 462.0;
  const double longest = max_link_length_km(radius, radius);
  EXPECT_NEAR(longest, 4507.8, 0.1);
  const double tangent = std::sqrt((radius * radius) - (r * r));
  EXPECT_NEAR(longest, 2.0 * tangent, 1e-9);
  const Eigen::Vector3d a(r, -tangent, 0.0);
  const Eigen::Vector3d b(r, tangent, 0.0);
  EXPECT_NEAR(a.norm(), radius, 1e-9);
  EXPECT_TRUE(has_line_of_sight(a, b));
  // Different radii add their own tangents.
  EXPECT_NEAR(max_link_length_km(6900.0, 7000.0),
              std::sqrt((6900.0 * 6900.0) - (r * r)) + std::sqrt((7000.0 * 7000.0) - (r * r)),
              1e-9);
  // A satellite at or below the layer has no meaningful longest link.
  EXPECT_EQ(max_link_length_km(r, 7000.0), 0.0);
  EXPECT_EQ(max_link_length_km(7000.0, 6000.0), 0.0);
}

// The east, north and up unit vectors at a site.
struct Enu {
  Eigen::Vector3d east;
  Eigen::Vector3d north;
  Eigen::Vector3d up;
};

Enu enu(const GroundSite& site) {
  const double sl = std::sin(site.latitude_rad);
  const double cl = std::cos(site.latitude_rad);
  const double so = std::sin(site.longitude_rad);
  const double co = std::cos(site.longitude_rad);
  return {.east = {-so, co, 0.0}, .north = {-sl * co, -sl * so, cl}, .up = {cl * co, cl * so, sl}};
}

// A satellite at `elevation_deg`, `range_km` from the site, toward the north.
Eigen::Vector3d toward_north(const GroundSite& site, double elevation_deg, double range_km) {
  const Enu e = enu(site);
  const double el = elevation_deg * kRadPerDeg;
  return site.position_ecef_km + (range_km * ((std::cos(el) * e.north) + (std::sin(el) * e.up)));
}

TEST(ElevationTest, OverheadAndOnTheHorizon) {
  for (const GroundSite& site :
       {make_ground_site("mid", 45.0, 10.0), make_ground_site("south", -33.9, 151.2),
        make_ground_site("equator", 0.0, 0.0), make_ground_site("north pole", 90.0, 0.0),
        make_ground_site("south pole", -90.0, 0.0)}) {
    const SiteGeometry g = site_geometry(site);
    const Enu e = enu(site);
    const LookAngle overhead = look_angle(g, site.position_ecef_km + (550.0 * e.up));
    EXPECT_NEAR(overhead.elevation_rad, std::numbers::pi / 2.0, 1e-12) << site.name;
    EXPECT_NEAR(overhead.slant_range_km, 550.0, 1e-9) << site.name;
    const LookAngle horizon = look_angle(g, site.position_ecef_km + (2000.0 * e.east));
    EXPECT_NEAR(horizon.elevation_rad, 0.0, 1e-12) << site.name;
    EXPECT_NEAR(horizon.slant_range_km, 2000.0, 1e-9) << site.name;
    const LookAngle below = look_angle(g, site.position_ecef_km - (100.0 * e.up));
    EXPECT_NEAR(below.elevation_rad, -std::numbers::pi / 2.0, 1e-12) << site.name;
  }
}

// On the ellipsoid's poles and equator the normal is the geocentric direction: overhead is
// straight out from the centre.
TEST(ElevationTest, PolesAndEquatorOfTheEllipsoid) {
  const double a = satsim::frames::kWgs84EquatorialRadiusKm;
  const double b = satsim::frames::kWgs84PolarRadiusKm;
  const SiteGeometry pole = site_geometry(make_ground_site("pole", 90.0, 0.0));
  EXPECT_NEAR(pole.position_ecef_km.z(), b, 1e-9);
  EXPECT_NEAR(look_angle(pole, {0.0, 0.0, b + 500.0}).elevation_rad, std::numbers::pi / 2.0, 1e-12);
  EXPECT_NEAR(look_angle(pole, {1000.0, 0.0, b}).elevation_rad, 0.0, 1e-12);
  const SiteGeometry equator = site_geometry(make_ground_site("equator", 0.0, 90.0));
  EXPECT_NEAR(equator.position_ecef_km.y(), a, 1e-9);
  EXPECT_NEAR(look_angle(equator, {0.0, a + 500.0, 0.0}).elevation_rad, std::numbers::pi / 2.0,
              1e-12);
  EXPECT_NEAR(look_angle(equator, {0.0, a, 1000.0}).elevation_rad, 0.0, 1e-12);
}

// At 45 deg latitude the ellipsoid normal is 0.19 deg from the geocentric direction. A
// satellite 0.001 deg above the 25 deg mask (above 500 km) measured from the normal is at
// about 24.81 deg from the geocentric horizontal: the geocentric approximation would put it
// below the mask.
TEST(ElevationTest, UsesTheEllipsoidNormalNotTheGeocentricDirection) {
  const GroundSite site = make_ground_site("mid", 45.0, 10.0);
  const Eigen::Vector3d satellite = toward_north(site, 25.001, 1200.0);
  const LookAngle look = look_angle(site_geometry(site), satellite);
  EXPECT_NEAR(look.elevation_rad, 25.001 * kRadPerDeg, 1e-12);
  EXPECT_NEAR(look.slant_range_km, 1200.0, 1e-9);

  const Eigen::Vector3d radial = site.position_ecef_km.normalized();
  const Eigen::Vector3d d = satellite - site.position_ecef_km;
  const double geocentric_rad = std::asin(radial.dot(d) / d.norm());
  EXPECT_NEAR((look.elevation_rad - geocentric_rad) / kRadPerDeg, 0.192, 0.002);
  EXPECT_GE(look.elevation_rad, elevation_mask_rad(550.0, site.latitude_rad));
  EXPECT_LT(geocentric_rad, elevation_mask_rad(550.0, site.latitude_rad));
}

TEST(ElevationMaskTest, TiersByHeightWithExplicitBoundaries) {
  const double lat = 40.0 * kRadPerDeg;
  EXPECT_DOUBLE_EQ(elevation_mask_rad(360.0, lat), 10.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(399.999, lat), 10.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(400.0, lat), 20.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(469.5, lat), 20.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(500.0, lat), 20.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(500.001, lat), 25.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(550.0, lat), 25.0 * kRadPerDeg);
}

TEST(ElevationMaskTest, HighLatitudeAllowanceIsOptionalAndStrictlyAbove62) {
  const ElevationMaskSettings on{.high_latitude_allowance = true};
  const double five = 5.0 * kRadPerDeg;
  // Off by default.
  EXPECT_DOUBLE_EQ(elevation_mask_rad(470.0, 70.0 * kRadPerDeg), 20.0 * kRadPerDeg);
  // On: strictly above 62 deg, north or south, any height.
  EXPECT_DOUBLE_EQ(elevation_mask_rad(470.0, 62.0 * kRadPerDeg, on), 20.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(470.0, 62.001 * kRadPerDeg, on), five);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(470.0, -62.001 * kRadPerDeg, on), five);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(550.0, 89.0 * kRadPerDeg, on), five);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(360.0, -70.0 * kRadPerDeg, on), five);
  EXPECT_DOUBLE_EQ(elevation_mask_rad(360.0, 61.0 * kRadPerDeg, on), 10.0 * kRadPerDeg);
}

TEST(ShellHeightTest, MeanOfPlaneMembersOnly) {
  satsim::constellation::Classification c;
  const auto add = [&](double height_km, std::optional<std::size_t> shell, bool member) {
    satsim::constellation::ClassifiedSatellite s;
    s.norad_cat_id = static_cast<std::int64_t>(c.satellites.size()) + 1;
    satsim::constellation::OrbitGeometry g;
    g.mean_normal_teme = Eigen::Vector3d::UnitZ();
    g.mean_height_km = height_km;
    s.geometry = g;
    s.shell_index = shell;
    if (member) {
      s.placement = satsim::constellation::PlaneAssignment{.plane_index = 0, .index_in_plane = 0};
      c.planes[*shell][0].members.push_back(c.satellites.size());
    } else {
      s.placement = satsim::constellation::ExclusionReason::kNoPlane;
    }
    c.satellites.push_back(s);
  };
  c.planes.resize(3);
  c.planes[0].resize(1);
  c.planes[1].resize(1);
  add(470.0, 0, true);
  add(472.0, 0, true);
  add(999.0, 0, false);  // In the shell's bands but in no plane: not counted.
  add(480.0, 1, true);
  const std::vector<std::optional<double>> heights = satsim::topology::shell_mean_heights_km(c);
  ASSERT_EQ(heights.size(), 3U);
  EXPECT_EQ(heights[0], std::optional<double>(471.0));
  EXPECT_EQ(heights[1], std::optional<double>(480.0));
  EXPECT_FALSE(heights[2].has_value());
}

}  // namespace
