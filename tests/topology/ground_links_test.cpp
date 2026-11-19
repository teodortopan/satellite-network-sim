#include "satsim/topology/ground_links.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "ideal.hpp"
#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/link_state.hpp"
#include "satsim/topology/physical_checks.hpp"
#include "satsim/topology/positions.hpp"
#include "support/synthetic.hpp"

namespace {

using satsim::constellation::Classification;
using satsim::ground::GroundSite;
using satsim::ground::make_ground_site;
using satsim::topology::GroundVisibility;
using satsim::topology::pick_serving_satellite;
using satsim::topology::VisibleSatellite;
using satsim::topology::testing::step_positions;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr satsim::time::CivilTime kEpoch{2026, 11, 18, 21, 15, 0.0};

// A classification of `count` satellites in one shell and one plane at `height_km`, except
// those listed in `excluded`.
Classification one_plane(std::size_t count, double height_km,
                         const std::vector<std::size_t>& excluded = {}) {
  Classification c;
  c.planes.resize(1);
  c.planes[0].resize(1);
  for (std::size_t i = 0; i < count; ++i) {
    satsim::constellation::ClassifiedSatellite s;
    s.norad_cat_id = 50000 + static_cast<std::int64_t>(i);
    satsim::constellation::OrbitGeometry g;
    g.mean_normal_teme = Eigen::Vector3d::UnitZ();
    g.mean_height_km = height_km;
    s.geometry = g;
    s.shell_index = 0;
    if (std::ranges::find(excluded, i) != excluded.end()) {
      s.placement = satsim::constellation::ExclusionReason::kNoLasers;
    } else {
      s.placement = satsim::constellation::PlaneAssignment{
          .plane_index = 0, .index_in_plane = c.planes[0][0].members.size()};
      c.planes[0][0].members.push_back(i);
    }
    c.satellites.push_back(s);
  }
  return c;
}

// A point at `elevation_deg` above the site's horizon, `range_km` away, toward the north.
Eigen::Vector3d at_elevation(const GroundSite& site, double elevation_deg, double range_km) {
  const double sl = std::sin(site.latitude_rad);
  const double cl = std::cos(site.latitude_rad);
  const double so = std::sin(site.longitude_rad);
  const double co = std::cos(site.longitude_rad);
  const Eigen::Vector3d north(-sl * co, -sl * so, cl);
  const Eigen::Vector3d up(cl * co, cl * so, sl);
  const double e = elevation_deg * kRadPerDeg;
  return site.position_ecef_km + (range_km * ((std::cos(e) * north) + (std::sin(e) * up)));
}

// Mask 20 deg (shell at 470 km). Satellite 0 just above it, 1 just below, 2 overhead but
// excluded, 3 has no position.
TEST(GroundVisibilityTest, JustAboveAndJustBelowTheMask) {
  const std::vector<GroundSite> sites = {make_ground_site("New York", 40.7128, -74.006)};
  const Classification c = one_plane(4, 470.0, {2});
  const GroundVisibility visibility(sites, c);
  EXPECT_DOUBLE_EQ(visibility.mask_rad(0, 0).value_or(-1.0), 20.0 * kRadPerDeg);
  EXPECT_FALSE(visibility.mask_rad(0, 1).has_value());
  const std::vector<std::optional<Eigen::Vector3d>> positions = {
      at_elevation(sites[0], 20.0001, 1200.0), at_elevation(sites[0], 19.9999, 1200.0),
      at_elevation(sites[0], 90.0, 500.0), std::nullopt};
  const auto visible =
      visibility.at(step_positions(satsim::constellation::testing::utc(kEpoch), positions));
  ASSERT_EQ(visible.size(), 1U);
  ASSERT_EQ(visible[0].size(), 1U);
  EXPECT_EQ(visible[0][0].satellite, 0U);
  EXPECT_EQ(visible[0][0].norad_cat_id, 50000);
  EXPECT_NEAR(visible[0][0].elevation_rad, 20.0001 * kRadPerDeg, 1e-12);
  EXPECT_NEAR(visible[0][0].slant_range_km, 1200.0, 1e-9);
  EXPECT_DOUBLE_EQ(visible[0][0].mask_rad, 20.0 * kRadPerDeg);
}

// With the high-latitude allowance a site above 62 deg uses 5 deg.
TEST(GroundVisibilityTest, HighLatitudeAllowance) {
  const std::vector<GroundSite> sites = {make_ground_site("Oulu", 65.0121, 25.4651),
                                         make_ground_site("Sydney", -33.9, 151.2)};
  const Classification c = one_plane(2, 470.0);
  const auto positions =
      step_positions(satsim::constellation::testing::utc(kEpoch),
                     {at_elevation(sites[0], 8.0, 1800.0), at_elevation(sites[1], 8.0, 1800.0)});
  const GroundVisibility normal(sites, c);
  EXPECT_TRUE(normal.at(positions)[0].empty());
  const GroundVisibility allowance(sites, c, {.high_latitude_allowance = true});
  EXPECT_DOUBLE_EQ(allowance.mask_rad(0, 0).value_or(-1.0), 5.0 * kRadPerDeg);
  EXPECT_DOUBLE_EQ(allowance.mask_rad(1, 0).value_or(-1.0), 20.0 * kRadPerDeg);
  const auto visible = allowance.at(positions);
  ASSERT_EQ(visible[0].size(), 1U);
  EXPECT_EQ(visible[0][0].satellite, 0U);
  EXPECT_TRUE(visible[1].empty());
}

TEST(ServingSatelliteTest, HighestElevationTiesToLowerCatalogueNumber) {
  EXPECT_FALSE(pick_serving_satellite({}).has_value());
  std::vector<VisibleSatellite> visible = {
      {.satellite = 3, .norad_cat_id = 103, .elevation_rad = 0.6},
      {.satellite = 1, .norad_cat_id = 101, .elevation_rad = 0.9},
      {.satellite = 2, .norad_cat_id = 102, .elevation_rad = 0.9},
      {.satellite = 0, .norad_cat_id = 100, .elevation_rad = 0.4},
  };
  EXPECT_EQ(pick_serving_satellite(visible).value_or(VisibleSatellite{}).norad_cat_id, 101);
  std::swap(visible[1], visible[2]);  // Order does not matter.
  EXPECT_EQ(pick_serving_satellite(visible).value_or(VisibleSatellite{}).norad_cat_id, 101);
  visible[3].elevation_rad = 1.2;
  EXPECT_EQ(pick_serving_satellite(visible).value_or(VisibleSatellite{}).norad_cat_id, 100);
}

// The z window and the quick test only skip work: on a full shell, the result equals a plain
// look_angle over every satellite, for sites at the poles, the equator, on both sides of the
// 62 deg limit and in between, with and without the allowance; and every result passes the
// mask check. (Kept small: GCC's ASan makes every Eigen call slow in the Debug build.)
TEST(GroundVisibilityTest, SameAsExhaustiveSearch) {
  const std::array<satsim::topology::testing::WalkerShell, 1> shells = {
      satsim::topology::testing::WalkerShell{.planes = 20,
                                             .per_plane = 15,
                                             .inclination_deg = 53.0,
                                             .height_km = 470.0,
                                             .phasing = 5,
                                             .epoch = kEpoch}};
  const auto ideal = satsim::topology::testing::ideal_shells(shells);
  const auto positions =
      step_positions(satsim::constellation::testing::utc(kEpoch), ideal.positions_ecef_km);
  std::vector<GroundSite> sites;
  for (const double lat : {-90.0, -62.5, -45.0, -20.0, 0.0, 20.0, 45.0, 61.9, 62.5, 90.0}) {
    for (const double lon : {-150.0, -30.0, 90.0}) {
      sites.push_back(make_ground_site("site", lat, lon));
    }
  }
  for (const bool allowance : {false, true}) {
    const satsim::topology::ElevationMaskSettings settings{.high_latitude_allowance = allowance};
    const GroundVisibility visibility(sites, ideal.classification, settings);
    const auto visible = visibility.at(positions);
    std::size_t total = 0;
    for (std::size_t s = 0; s < sites.size(); ++s) {
      const auto g = satsim::topology::site_geometry(sites[s]);
      const double mask =
          satsim::topology::elevation_mask_rad(470.0, sites[s].latitude_rad, settings);
      std::vector<std::size_t> expected;
      for (std::size_t i = 0; i < ideal.positions_ecef_km.size(); ++i) {
        if (satsim::topology::look_angle(g, *ideal.positions_ecef_km[i]).elevation_rad >= mask) {
          expected.push_back(i);
        }
      }
      std::vector<std::size_t> actual;
      for (const VisibleSatellite& v : visible[s]) {
        actual.push_back(v.satellite);
      }
      EXPECT_EQ(actual, expected) << sites[s].latitude_rad / kRadPerDeg;
      total += actual.size();
    }
    EXPECT_GT(total, sites.size());  // The test sees something.
    satsim::topology::LinkPlan no_lasers;
    no_lasers.lasers.resize(ideal.classification.satellites.size());
    const satsim::topology::StepLinks no_links{.utc = positions.utc, .states = {}};
    EXPECT_EQ(satsim::topology::run_physical_checks(ideal.classification, no_lasers, no_links,
                                                    positions, {}, sites, visible, settings)
                  .total(),
              0U);
  }
}

}  // namespace
