// Physical plausibility of propagated states: radius, speed, period and the latitude band,
// against the hand-traced example of docs/phase-1-orbits.md section 5 and real Starlink
// element sets from tests/orbit/data/omm_fixture.json.

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <variant>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::orbit::ElementSet;
using satsim::orbit::load_omm_json;
using satsim::orbit::PropagationError;
using satsim::orbit::Propagator;
using satsim::orbit::TemeState;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
// WGS-72 gravitational parameter, the one SGP4 uses (km^3/s^2).
constexpr double kMuKm3PerS2 = 398600.8;

// Semi-major axis of a two-body orbit with this mean motion: n^2 a^3 = mu.
double two_body_semi_major_axis_km(double mean_motion_rev_per_day) {
  const double n_rad_per_s = mean_motion_rev_per_day * kTwoPi / 86400.0;
  return std::cbrt(kMuKm3PerS2 / (n_rad_per_s * n_rad_per_s));
}

TemeState state_at(const Propagator& p, double minutes) {
  const auto result = p.propagate_minutes_since_epoch(minutes);
  const auto* error = std::get_if<PropagationError>(&result);
  EXPECT_EQ(error, nullptr) << (error != nullptr ? satsim::orbit::describe(*error) : "") << " at "
                            << minutes << " min";
  return error == nullptr ? std::get<TemeState>(result) : TemeState{};
}

// Extremes over one orbit (one Kozai mean-motion period), sampled every minute.
struct OrbitSummary {
  double min_radius_km = 1e300;
  double max_radius_km = 0.0;
  double min_speed_km_s = 1e300;
  double max_speed_km_s = 0.0;
  double max_vis_viva_error_km_s = 0.0;  // |v| against sqrt(mu (2/r - 1/a)).
  double max_abs_z_over_r = 0.0;
  double max_inclination_error_rad = 0.0;  // Inclination from r x v against the element set.
  double return_distance_km = 0.0;         // |r(period) - r(0)|.
  double half_period_distance_km = 0.0;    // |r(period / 2) - r(0)|.
};

OrbitSummary summarise(const ElementSet& e) {
  OrbitSummary s;
  auto created = Propagator::create(e);
  const auto* p = std::get_if<Propagator>(&created);
  if (p == nullptr) {
    ADD_FAILURE() << "create failed for " << e.norad_cat_id;
    return s;
  }
  const double a_km = two_body_semi_major_axis_km(e.mean_motion_rev_per_day);
  const double period_min = 1440.0 / e.mean_motion_rev_per_day;
  const auto samples = static_cast<int>(std::ceil(period_min));
  for (int i = 0; i <= samples; ++i) {
    const double t = std::min(static_cast<double>(i), period_min);
    const TemeState x = state_at(*p, t);
    const double r = x.position_km.norm();
    const double v = x.velocity_km_s.norm();
    s.min_radius_km = std::min(s.min_radius_km, r);
    s.max_radius_km = std::max(s.max_radius_km, r);
    s.min_speed_km_s = std::min(s.min_speed_km_s, v);
    s.max_speed_km_s = std::max(s.max_speed_km_s, v);
    const double vis_viva = std::sqrt(kMuKm3PerS2 * (2.0 / r - 1.0 / a_km));
    s.max_vis_viva_error_km_s = std::max(s.max_vis_viva_error_km_s, std::fabs(v - vis_viva));
    s.max_abs_z_over_r = std::max(s.max_abs_z_over_r, std::fabs(x.position_km.z()) / r);
    const Eigen::Vector3d h = x.position_km.cross(x.velocity_km_s);
    const double inclination = std::acos(h.z() / h.norm());
    s.max_inclination_error_rad =
        std::max(s.max_inclination_error_rad, std::fabs(inclination - e.inclination_rad));
  }
  const Eigen::Vector3d r0 = state_at(*p, 0.0).position_km;
  s.return_distance_km = (state_at(*p, period_min).position_km - r0).norm();
  s.half_period_distance_km = (state_at(*p, period_min / 2.0).position_km - r0).norm();
  return s;
}

// Margins, each about twice the largest value seen on the seven orbits below and far below
// what a unit or frame mistake causes (hundreds to thousands of kilometres, or degrees):
//
// Radius against the two-body semi-major axis from the mean motion: SGP4's own semi-major
// axis differs from it by O(J2 (Re/a)^2), about 1e-3 or 5 km, and the J2 short-period terms
// move the radius by a few more km. Largest seen: 8.3 km beyond a*e.
constexpr double kRadiusMarginKm = 15.0;
// |v| against vis-viva sqrt(mu (2/r - 1/a)): the same J2 terms, about 1e-3 of 7.6 km/s.
// Largest seen: 0.0059 km/s.
constexpr double kVisVivaMarginKmS = 0.012;
// Inclination of the plane of r and v against the mean inclination: the J2 short-period
// oscillation of inclination, about (3/4) J2 (Re/p)^2 sin(i) cos(i) = 3e-4 rad. Largest seen:
// 3.6e-4 rad.
constexpr double kInclinationMarginRad = 1e-3;
// Highest |z|/r over an orbit is sin(osculating inclination): above sin(i) by at most
// cos(i) * 3.6e-4. Below it by sampling every minute (the satellite moves up to 3.9 degrees
// per minute, so the best sample can miss the peak by 1 - cos(1.95 deg) = 5.8e-4 of it) and
// by the same oscillation. Largest seen: 3.9e-4 below.
constexpr double kZOverRAboveMargin = 1e-3;
constexpr double kZOverRBelowMargin = 2e-3;
// After one period of the Kozai mean motion the satellite is back where it started but for
// the J2 drift of the node and of the argument of latitude over one orbit, a few tenths of a
// degree, i.e. tens of km. Largest seen: 40.3 km. A period wrong by 1 % would be ~430 km off.
constexpr double kReturnDistanceKm = 80.0;
// Half a period later it is on the far side: about 2a apart, give or take the radius margin
// at both ends. Largest shortfall seen: 4.5 km.
constexpr double kHalfPeriodShortfallKm = 2.0 * kRadiusMarginKm;

void expect_plausible(const ElementSet& e, const OrbitSummary& s) {
  const double a_km = two_body_semi_major_axis_km(e.mean_motion_rev_per_day);
  const double radius_margin_km = a_km * e.eccentricity + kRadiusMarginKm;
  EXPECT_GT(s.min_radius_km, a_km - radius_margin_km);
  EXPECT_LT(s.max_radius_km, a_km + radius_margin_km);
  EXPECT_LT(s.max_vis_viva_error_km_s, kVisVivaMarginKmS);
  EXPECT_LT(s.max_inclination_error_rad, kInclinationMarginRad);
  const double sin_i = std::sin(e.inclination_rad);
  EXPECT_LT(s.max_abs_z_over_r, sin_i + kZOverRAboveMargin);
  EXPECT_GT(s.max_abs_z_over_r, sin_i - kZOverRBelowMargin);
  EXPECT_LT(s.return_distance_km, kReturnDistanceKm);
  EXPECT_GT(s.half_period_distance_km, 2.0 * a_km - kHalfPeriodShortfallKm);
}

// docs/phase-1-orbits.md, section 5: a circular orbit 550 km up has radius
// 6378.137 + 550 = 6928.137 km, speed sqrt(398600.4418 / 6928.137) = 7.585 km/s and period
// 5739 s, i.e. 86400 / 5739 = 15.0549 revolutions per day. (SGP4's WGS-72 mu, 398600.8, moves
// the radius for that period by 2 m.)
TEST(PhysicalSanityTest, HandTracedCircularOrbitAt550Km) {
  ElementSet e;
  e.norad_cat_id = 1;
  e.epoch = satsim::time::CivilTime{2026, 11, 17, 0, 0, 0.0};
  e.mean_motion_rev_per_day = 86400.0 / 5739.0;
  e.eccentricity = 0.0;
  e.inclination_rad = deg_to_rad(53.0);
  const OrbitSummary s = summarise(e);
  EXPECT_NEAR(two_body_semi_major_axis_km(e.mean_motion_rev_per_day), 6928.137, 0.1);
  // Seen: radius 6921.0 to 6933.0 km, speed 7.5766 to 7.5900 km/s (7.585 is rounded to
  // 0.0005), return after 5739 s 28.9 km.
  EXPECT_NEAR(s.min_radius_km, 6928.137, kRadiusMarginKm);
  EXPECT_NEAR(s.max_radius_km, 6928.137, kRadiusMarginKm);
  EXPECT_NEAR(s.min_speed_km_s, 7.585, 2 * kVisVivaMarginKmS);
  EXPECT_NEAR(s.max_speed_km_s, 7.585, 2 * kVisVivaMarginKmS);
  expect_plausible(e, s);
}

// Real Starlink element sets (standard and supplemental), 250 to 480 km up, inclinations 30.5
// to 70 degrees.
TEST(PhysicalSanityTest, FixtureStarlinkElementSets) {
  const auto loaded = load_omm_json(std::filesystem::path("tests/orbit/data/omm_fixture.json"));
  ASSERT_FALSE(loaded.file_error.has_value());
  ASSERT_EQ(loaded.element_sets.size(), 6U);
  for (const ElementSet& e : loaded.element_sets) {
    SCOPED_TRACE(e.object_name);
    expect_plausible(e, summarise(e));
  }
}

}  // namespace
