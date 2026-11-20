#include "satsim/topology/ground_links.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
// 62 deg limit and in between, with and without the allowance and under each override; and
// every result passes the mask check. (Kept small: GCC's ASan makes every Eigen call slow in the
// Debug build.)
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
  // The FCC masks, with the allowance, and the two overrides (Phase 3 spec 4.4).
  std::vector<satsim::topology::ElevationMaskSettings> all_settings(4);
  all_settings[1].high_latitude_allowance = true;
  all_settings[2].min_elevation_rad = 15.0 * kRadPerDeg;
  all_settings[3].max_ground_range_km = 1500.0;
  for (const auto& settings : all_settings) {
    const GroundVisibility visibility(sites, ideal.classification, settings);
    const auto visible = visibility.at(positions);
    std::size_t total = 0;
    for (std::size_t s = 0; s < sites.size(); ++s) {
      const auto g = satsim::topology::site_geometry(sites[s]);
      const double mask =
          satsim::topology::elevation_mask_rad(470.0, sites[s].latitude_rad, settings);
      std::vector<std::size_t> expected;
      for (std::size_t i = 0; i < ideal.positions_ecef_km.size(); ++i) {
        if (satsim::topology::ground_link_allowed(
                satsim::topology::look_angle(g, *ideal.positions_ecef_km[i]), mask, settings)) {
          expected.push_back(i);
        }
      }
      std::vector<std::size_t> actual;
      for (const VisibleSatellite& v : visible[s]) {
        actual.push_back(v.satellite);
      }
      EXPECT_EQ(actual, expected) << sites[s].latitude_rad / kRadPerDeg;
      if (settings.max_ground_range_km) {
        for (const VisibleSatellite& v : visible[s]) {
          EXPECT_EQ(v.mask_rad, -std::numbers::pi / 2.0);
        }
      }
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

using satsim::topology::ElevationMaskSettings;
using satsim::topology::look_angle;
using satsim::topology::site_geometry;
using satsim::topology::time_left_in_view_s;
using Positions = std::vector<std::optional<Eigen::Vector3d>>;

// One satellite at `point` from `site`, in a one-plane shell at 470 km (FCC mask 20 deg).
std::vector<std::vector<VisibleSatellite>> visible_with(const GroundSite& site,
                                                        const Eigen::Vector3d& point,
                                                        const ElevationMaskSettings& settings) {
  const Classification c = one_plane(1, 470.0);
  const std::vector<GroundSite> sites = {site};
  const GroundVisibility visibility(sites, c, settings);
  return visibility.at(step_positions(satsim::constellation::testing::utc(kEpoch), {point}));
}

TEST(MaskSettingsTest, Checks) {
  EXPECT_EQ(satsim::topology::check_mask_settings({}), std::nullopt);
  ElevationMaskSettings s;
  s.min_elevation_rad = 0.0;
  EXPECT_EQ(satsim::topology::check_mask_settings(s), std::nullopt);
  s.min_elevation_rad = std::nextafter(std::numbers::pi / 2.0, 0.0);
  EXPECT_EQ(satsim::topology::check_mask_settings(s), std::nullopt);
  for (const double bad : {-1e-12, std::numbers::pi / 2.0, std::nan("")}) {
    s.min_elevation_rad = bad;
    EXPECT_NE(satsim::topology::check_mask_settings(s), std::nullopt) << bad;
  }
  s = {};
  s.max_ground_range_km = 1089.686;
  EXPECT_EQ(satsim::topology::check_mask_settings(s), std::nullopt);
  for (const double bad : {0.0, -5.0, std::numeric_limits<double>::infinity()}) {
    s.max_ground_range_km = bad;
    EXPECT_NE(satsim::topology::check_mask_settings(s), std::nullopt) << bad;
  }
  s.max_ground_range_km = 1000.0;
  s.min_elevation_rad = 0.5;
  EXPECT_NE(satsim::topology::check_mask_settings(s), std::nullopt);
  s.min_elevation_rad.reset();
  s.high_latitude_allowance = true;
  EXPECT_NE(satsim::topology::check_mask_settings(s), std::nullopt);
}

// One minimum elevation replaces the FCC masks for every height and latitude: a satellite at
// 15 deg (below the 20 deg FCC mask) is visible with the minimum at its exact elevation and
// just below it, not just above it.
TEST(MaskOverrideTest, MinimumElevationAtJustAboveAndJustBelow) {
  const GroundSite site = make_ground_site("New York", 40.7128, -74.006);
  const Eigen::Vector3d point = at_elevation(site, 15.0, 1500.0);
  const double e = look_angle(site_geometry(site), point).elevation_rad;
  EXPECT_TRUE(visible_with(site, point, {})[0].empty());
  for (const double height_km : {350.0, 470.0, 550.0}) {
    EXPECT_EQ(satsim::topology::elevation_mask_rad(height_km, 1.3, {.min_elevation_rad = 0.7}),
              0.7);
  }
  const auto at = visible_with(site, point, {.min_elevation_rad = e});
  ASSERT_EQ(at[0].size(), 1U);
  EXPECT_EQ(at[0][0].mask_rad, e);
  EXPECT_EQ(visible_with(site, point, {.min_elevation_rad = std::nextafter(e, 0.0)})[0].size(), 1U);
  EXPECT_TRUE(visible_with(site, point, {.min_elevation_rad = std::nextafter(e, 1.0)})[0].empty());
}

// Hypatia's rule: in range if the slant range is at most the limit, with no elevation test. A
// satellite at 5 deg (far below any FCC mask) 1000 km away is visible with the limit at its
// exact slant range and just above it, not just below it; one overhead but 1200 km up is not
// visible under a 1100 km limit, however high it is.
TEST(MaskOverrideTest, MaxGroundRangeAtJustAboveAndJustBelow) {
  const GroundSite site = make_ground_site("New York", 40.7128, -74.006);
  const Eigen::Vector3d point = at_elevation(site, 5.0, 1000.0);
  const double range = look_angle(site_geometry(site), point).slant_range_km;
  EXPECT_NEAR(range, 1000.0, 1e-9);
  const auto at = visible_with(site, point, {.max_ground_range_km = range});
  ASSERT_EQ(at[0].size(), 1U);
  EXPECT_EQ(at[0][0].mask_rad, -std::numbers::pi / 2.0);
  EXPECT_EQ(at[0][0].slant_range_km, range);
  EXPECT_EQ(
      visible_with(site, point, {.max_ground_range_km = std::nextafter(range, 2000.0)})[0].size(),
      1U);
  EXPECT_TRUE(
      visible_with(site, point, {.max_ground_range_km = std::nextafter(range, 0.0)})[0].empty());
  EXPECT_TRUE(
      visible_with(site, at_elevation(site, 90.0, 1200.0), {.max_ground_range_km = 1100.0})[0]
          .empty());
}

// The physical checks under each override: the elevation against the one minimum, the
// distance against the range.
TEST(MaskOverrideTest, PhysicalChecksFollowTheOverride) {
  const GroundSite site = make_ground_site("New York", 40.7128, -74.006);
  const Classification c = one_plane(1, 470.0);
  const std::vector<GroundSite> sites = {site};
  const auto positions = step_positions(satsim::constellation::testing::utc(kEpoch),
                                        {at_elevation(site, 5.0, 1000.0)});
  const std::vector<std::vector<VisibleSatellite>> claimed = {{VisibleSatellite{.satellite = 0}}};
  satsim::topology::LinkPlan no_lasers;
  no_lasers.lasers.resize(1);
  const satsim::topology::StepLinks no_links{.utc = positions.utc, .states = {}};
  const auto mask_failures = [&](const ElevationMaskSettings& settings) {
    return satsim::topology::run_physical_checks(c, no_lasers, no_links, positions, {}, sites,
                                                 claimed, settings)
        .mask;
  };
  EXPECT_EQ(mask_failures({}), 1U);  // 5 deg < 20 deg.
  EXPECT_EQ(mask_failures({.min_elevation_rad = 4.0 * kRadPerDeg}), 0U);
  EXPECT_EQ(mask_failures({.min_elevation_rad = 6.0 * kRadPerDeg}), 1U);
  EXPECT_EQ(mask_failures({.max_ground_range_km = 1000.001}), 0U);
  EXPECT_EQ(mask_failures({.max_ground_range_km = 999.999}), 1U);
}

// A satellite on a circular orbit of radius r = 6378.137 + 550 km in the equatorial plane,
// passing straight over a site on the equator at longitude 0 (the site at R = 6378.137 km on
// the x axis, its vertical the x axis): at central angle theta from the site its elevation e
// has tan e = (r cos theta - R) / (r sin theta), and e = m where theta = acos(R cos m / r) - m.
// For m = 25 deg: R cos m / r = 5780.53 / 6928.137 = 0.834357, acos = 33.4508 deg, so
// theta_m = 8.4508 deg. With a period of 5739 s (360 / 5739 deg/s) that is 134.72 s from
// overhead to the mask: steps of 1 s are in view at k = 0..134 and out at 135 (135 s); steps of
// 10 s in view at k = 0..13 and out at 14 (140 s). Starting 5 deg before overhead it is
// (8.4508 + 5) / 360 x 5739 = 214.43 s: 215 s at 1 s steps.
Positions overhead_pass(double start_deg, double step_s, std::size_t count) {
  constexpr double kRadiusKm = 6378.137 + 550.0;
  const double rate_rad_s = 2.0 * std::numbers::pi / 5739.0;
  Positions out;
  for (std::size_t k = 0; k < count; ++k) {
    const double theta = (start_deg * kRadPerDeg) + (rate_rad_s * static_cast<double>(k) * step_s);
    out.emplace_back(
        Eigen::Vector3d(kRadiusKm * std::cos(theta), kRadiusKm * std::sin(theta), 0.0));
  }
  return out;
}

TEST(TimeLeftInViewTest, OverheadPassMatchesTheGeometry) {
  const auto site = site_geometry(make_ground_site("equator", 0.0, 0.0));
  const double m = 25.0 * kRadPerDeg;
  EXPECT_EQ(satsim::topology::view_steps(1.0), 900U);
  EXPECT_EQ(time_left_in_view_s(site, m, overhead_pass(0.0, 1.0, 900), 1.0), 135.0);
  EXPECT_EQ(time_left_in_view_s(site, m, overhead_pass(0.0, 10.0, 90), 10.0), 140.0);
  EXPECT_EQ(time_left_in_view_s(site, m, overhead_pass(-5.0, 1.0, 900), 1.0), 215.0);
  // 20 deg before overhead (beyond theta_m) it has not risen: zero.
  EXPECT_EQ(time_left_in_view_s(site, m, overhead_pass(-20.0, 1.0, 900), 1.0), 0.0);
}

// In view at step 3 exactly at the mask (its own elevation), below at step 4: 4 steps of 15 s.
// With the mask the next double up, nothing is in view: 0.
TEST(TimeLeftInViewTest, AtTheMaskBoundary) {
  const GroundSite ground_site = make_ground_site("New York", 40.7128, -74.006);
  const auto site = site_geometry(ground_site);
  const Eigen::Vector3d high = at_elevation(ground_site, 30.0, 900.0);
  const Eigen::Vector3d low = at_elevation(ground_site, 10.0, 1800.0);
  const Positions future = {high, high, high, high, low, high};
  const double e = look_angle(site, high).elevation_rad;
  EXPECT_EQ(time_left_in_view_s(site, e, future, 15.0), 60.0);
  EXPECT_EQ(time_left_in_view_s(site, std::nextafter(e, 1.0), future, 15.0), 0.0);
  // The range rule the same way, on distance.
  const double range = look_angle(site, high).slant_range_km;
  EXPECT_EQ(time_left_in_view_s(site, 0.0, future, 15.0, 900.0, {.max_ground_range_km = range}),
            60.0);
  EXPECT_EQ(time_left_in_view_s(site, 0.0, future, 15.0, 900.0,
                                {.max_ground_range_km = std::nextafter(range, 0.0)}),
            0.0);
}

// Still in view at the horizon counts as the horizon. With 7 s steps the horizon of 900 s is
// read through k = 128 (896 s; view_steps = ceil(900 / 7) = 129): out at k = 128 gives 896 s,
// in view through k = 128 gives 900 s, not 903.
TEST(TimeLeftInViewTest, CappedAtTheHorizon) {
  const GroundSite ground_site = make_ground_site("New York", 40.7128, -74.006);
  const auto site = site_geometry(ground_site);
  const Eigen::Vector3d high = at_elevation(ground_site, 60.0, 600.0);
  const Eigen::Vector3d low = at_elevation(ground_site, 5.0, 2000.0);
  const double m = 20.0 * kRadPerDeg;
  EXPECT_EQ(time_left_in_view_s(site, m, Positions(900, high), 1.0), 900.0);
  EXPECT_EQ(time_left_in_view_s(site, m, Positions(2000, high), 1.0), 900.0);
  EXPECT_EQ(satsim::topology::view_steps(7.0), 129U);
  Positions future(130, high);
  EXPECT_EQ(time_left_in_view_s(site, m, future, 7.0), 900.0);
  future[129] = low;  // Past the horizon: not read.
  EXPECT_EQ(time_left_in_view_s(site, m, future, 7.0), 900.0);
  future[128] = low;
  EXPECT_EQ(time_left_in_view_s(site, m, future, 7.0), 896.0);
  // Another horizon.
  EXPECT_EQ(time_left_in_view_s(site, m, Positions(10, high), 15.0, 60.0), 60.0);
  // Fewer positions than the horizon needs: the view ends after the last.
  EXPECT_EQ(time_left_in_view_s(site, m, Positions(3, high), 15.0), 45.0);
}

TEST(TimeLeftInViewTest, ZeroWhenNotInViewAndEndsAtAFailedPosition) {
  const GroundSite ground_site = make_ground_site("New York", 40.7128, -74.006);
  const auto site = site_geometry(ground_site);
  const Eigen::Vector3d high = at_elevation(ground_site, 60.0, 600.0);
  const Eigen::Vector3d low = at_elevation(ground_site, 5.0, 2000.0);
  const double m = 20.0 * kRadPerDeg;
  Positions future(900, high);
  future[0] = low;
  EXPECT_EQ(time_left_in_view_s(site, m, future, 1.0), 0.0);
  future[0] = high;
  future[5].reset();
  EXPECT_EQ(time_left_in_view_s(site, m, future, 1.0), 5.0);
  future[0].reset();
  EXPECT_EQ(time_left_in_view_s(site, m, future, 1.0), 0.0);
  EXPECT_EQ(time_left_in_view_s(site, m, {}, 1.0), 0.0);
}

// Through GroundVisibility: its own mask for the satellite's shell (20 deg at 470 km), for
// every satellite at() lists at once. Satellite 0 at 30 deg for 3 steps then 10 deg (3 steps
// of 15 s: 45 s); 1 at 30 deg throughout (the horizon); 2 excluded (0, overhead or not); 3 at
// 10 deg (0, and not listed by at()).
TEST(TimeLeftInViewTest, ForEverySatelliteASiteSees) {
  const GroundSite ground_site = make_ground_site("New York", 40.7128, -74.006);
  const std::vector<GroundSite> sites = {ground_site};
  const Classification c = one_plane(4, 470.0, {2});
  const GroundVisibility visibility(sites, c);
  const Eigen::Vector3d high = at_elevation(ground_site, 30.0, 900.0);
  const Eigen::Vector3d low = at_elevation(ground_site, 10.0, 1800.0);
  const Eigen::Vector3d overhead = at_elevation(ground_site, 90.0, 500.0);
  std::vector<satsim::topology::StepPositions> steps;
  for (std::size_t k = 0; k < satsim::topology::view_steps(15.0); ++k) {
    steps.push_back(step_positions(satsim::constellation::testing::utc(kEpoch),
                                   {k < 3 ? high : low, high, overhead, low}));
  }
  const auto visible = visibility.at(steps[0]);
  ASSERT_EQ(visible[0].size(), 2U);
  EXPECT_EQ(visibility.times_left_in_view_s(0, visible[0], steps, 15.0),
            (std::vector<double>{45.0, 900.0}));
  EXPECT_EQ(visibility.time_left_in_view_s(0, 2, Positions(60, overhead), 15.0), 0.0);
  EXPECT_EQ(visibility.time_left_in_view_s(0, 3, Positions(60, low), 15.0), 0.0);
  EXPECT_EQ(visibility.time_left_in_view_s(0, 1, Positions(60, high), 15.0), 900.0);
  EXPECT_EQ(visibility.time_left_in_view_s(0, 99, Positions(60, high), 15.0), 0.0);
  // Under the range rule the range decides: high is 900 km away, low 1800 km.
  const GroundVisibility by_range(sites, c, {.max_ground_range_km = 1000.0});
  const auto in_range = by_range.at(steps[0]);
  ASSERT_EQ(in_range[0].size(), 2U);
  EXPECT_EQ(by_range.times_left_in_view_s(0, in_range[0], steps, 15.0),
            (std::vector<double>{45.0, 900.0}));
}

}  // namespace
