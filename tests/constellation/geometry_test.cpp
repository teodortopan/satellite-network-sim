#include "satsim/constellation/geometry.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <variant>
#include <vector>

#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_set.hpp"
#include "satsim/orbit/propagator.hpp"
#include "satsim/time/civil_time.hpp"
#include "satsim/time/utc_time.hpp"
#include "synthetic.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::rad_to_deg;
using satsim::constellation::argument_of_latitude_rad;
using satsim::constellation::circular_mean_rad;
using satsim::constellation::cluster_by_circular_gaps;
using satsim::constellation::GeometrySampling;
using satsim::constellation::inclination_rad;
using satsim::constellation::Launch;
using satsim::constellation::launch_of;
using satsim::constellation::measure_orbit_geometry;
using satsim::constellation::orbit_normal_teme;
using satsim::constellation::OrbitGeometry;
using satsim::constellation::raan_teme_rad;
using satsim::constellation::wrap_pi;
using satsim::constellation::testing::utc;
using satsim::constellation::testing::walker_element_sets;
using satsim::constellation::testing::WalkerShell;
using satsim::orbit::ElementSet;
using satsim::orbit::PropagationError;
using satsim::orbit::Propagator;
using satsim::time::CivilTime;

constexpr double kPi = std::numbers::pi;
constexpr double kExact = 1e-12;

// The unit normal of the plane with inclination i and RAAN raan: R3(-raan) R1(-i) z.
Eigen::Vector3d normal(double i_rad, double raan_rad) {
  return {std::sin(i_rad) * std::sin(raan_rad), -std::sin(i_rad) * std::cos(raan_rad),
          std::cos(i_rad)};
}

// A position at argument of latitude u in that plane, radius 7000 km.
Eigen::Vector3d position(double i_rad, double raan_rad, double u_rad) {
  const Eigen::Vector3d node(std::cos(raan_rad), std::sin(raan_rad), 0.0);
  const Eigen::Vector3d ahead = normal(i_rad, raan_rad).cross(node);
  return 7000.0 * (std::cos(u_rad) * node + std::sin(u_rad) * ahead);
}

TEST(GeometryTest, EquatorialProgradeOrbit) {
  const Eigen::Vector3d r(7000.0, 0.0, 0.0);
  const Eigen::Vector3d v(0.0, 7.5, 0.0);
  const Eigen::Vector3d n = orbit_normal_teme(r, v);
  EXPECT_NEAR((n - Eigen::Vector3d::UnitZ()).norm(), 0.0, kExact);
  EXPECT_NEAR(inclination_rad(n), 0.0, kExact);
  EXPECT_EQ(raan_teme_rad(n), 0.0);  // Undefined node: 0 by convention, node along x.
  EXPECT_NEAR(argument_of_latitude_rad(r, n), 0.0, kExact);
  EXPECT_NEAR(argument_of_latitude_rad(Eigen::Vector3d(0.0, 7000.0, 0.0), n), kPi / 2, kExact);
  EXPECT_NEAR(argument_of_latitude_rad(Eigen::Vector3d(0.0, -7000.0, 0.0), n), 3 * kPi / 2, kExact);
}

TEST(GeometryTest, EquatorialRetrogradeOrbitHasInclinationPi) {
  const Eigen::Vector3d n = orbit_normal_teme({7000.0, 0.0, 0.0}, {0.0, -7.5, 0.0});
  EXPECT_NEAR(inclination_rad(n), kPi, kExact);
  EXPECT_EQ(raan_teme_rad(n), 0.0);
}

// Moving north from the +x axis: the node is at +x (RAAN 0) and the satellite is at it.
TEST(GeometryTest, PolarOrbit) {
  const Eigen::Vector3d r(7000.0, 0.0, 0.0);
  const Eigen::Vector3d n = orbit_normal_teme(r, {0.0, 0.0, 7.5});
  EXPECT_NEAR((n - Eigen::Vector3d(0.0, -1.0, 0.0)).norm(), 0.0, kExact);
  EXPECT_NEAR(inclination_rad(n), kPi / 2, kExact);
  EXPECT_NEAR(raan_teme_rad(n), 0.0, kExact);
  EXPECT_NEAR(argument_of_latitude_rad(r, n), 0.0, kExact);
  EXPECT_NEAR(argument_of_latitude_rad({0.0, 0.0, 7000.0}, n), kPi / 2, kExact);
  EXPECT_NEAR(argument_of_latitude_rad({-7000.0, 0.0, 0.0}, n), kPi, kExact);
}

// Moving north from the +y axis: node at +y, RAAN 90 degrees.
TEST(GeometryTest, PolarOrbitWithNodeOnY) {
  const Eigen::Vector3d n = orbit_normal_teme({0.0, 7000.0, 0.0}, {0.0, 0.0, 7.5});
  EXPECT_NEAR(raan_teme_rad(n), kPi / 2, kExact);
}

// Normal and positions built from known angles, prograde and retrograde, and a node near 0/2pi.
TEST(GeometryTest, KnownInclinationNodeAndArgumentOfLatitude) {
  struct Case {
    double i_deg;
    double raan_deg;
    double u_deg;
  };
  for (const Case c : {Case{53.0, 300.0, 10.0}, Case{97.6, 10.0, 200.0}, Case{43.0, 359.9, 359.5},
                       Case{70.0, 0.05, 90.0}}) {
    const double i = deg_to_rad(c.i_deg);
    const double raan = deg_to_rad(c.raan_deg);
    const Eigen::Vector3d r = position(i, raan, deg_to_rad(c.u_deg));
    // A velocity along the direction of motion (90 degrees ahead in the plane).
    const Eigen::Vector3d v = position(i, raan, deg_to_rad(c.u_deg + 90.0)) / 1000.0;
    const Eigen::Vector3d n = orbit_normal_teme(r, v);
    EXPECT_NEAR((n - normal(i, raan)).norm(), 0.0, 1e-12) << c.i_deg;
    EXPECT_NEAR(rad_to_deg(inclination_rad(n)), c.i_deg, 1e-10) << c.i_deg;
    EXPECT_NEAR(rad_to_deg(raan_teme_rad(n)), c.raan_deg, 1e-10) << c.i_deg;
    EXPECT_NEAR(rad_to_deg(argument_of_latitude_rad(r, n)), c.u_deg, 1e-10) << c.i_deg;
  }
}

TEST(GeometryTest, WrapPi) {
  EXPECT_NEAR(wrap_pi(0.1), 0.1, kExact);
  EXPECT_NEAR(wrap_pi(2 * kPi - 0.1), -0.1, kExact);
  EXPECT_NEAR(wrap_pi(-2 * kPi + 0.1), 0.1, kExact);
  EXPECT_NEAR(wrap_pi(kPi), -kPi, kExact);  // [-pi, pi)
}

TEST(GeometryTest, CircularMeanAcrossZero) {
  const std::vector<double> angles = {deg_to_rad(359.0), deg_to_rad(1.0), deg_to_rad(3.0)};
  const std::optional<double> mean = circular_mean_rad(angles);
  if (!mean) {
    FAIL() << "no mean";
  }
  EXPECT_NEAR(rad_to_deg(*mean), 1.0, 1e-9);
  const std::vector<double> before_zero = {deg_to_rad(358.0), deg_to_rad(359.0)};
  EXPECT_NEAR(rad_to_deg(circular_mean_rad(before_zero).value_or(0.0)), 358.5, 1e-9);
}

TEST(GeometryTest, CircularMeanUndefined) {
  EXPECT_FALSE(circular_mean_rad({}).has_value());
  const std::vector<double> opposite = {0.0, kPi};
  EXPECT_FALSE(circular_mean_rad(opposite).has_value());
}

std::vector<std::vector<std::size_t>> cluster_deg(const std::vector<double>& degrees,
                                                  double threshold_deg,
                                                  std::vector<std::int64_t> keys = {}) {
  std::vector<double> radians;
  radians.reserve(degrees.size());
  for (const double d : degrees) {
    radians.push_back(deg_to_rad(d));
  }
  if (keys.empty()) {
    for (std::size_t i = 0; i < degrees.size(); ++i) {
      keys.push_back(static_cast<std::int64_t>(i));
    }
  }
  return cluster_by_circular_gaps(radians, keys, deg_to_rad(threshold_deg));
}

using Groups = std::vector<std::vector<std::size_t>>;

TEST(ClusterTest, SplitsAtGapsAboveTheThreshold) {
  EXPECT_EQ(cluster_deg({10.0, 10.5, 50.0, 11.0, 49.0}, 2.0), (Groups{{0, 1, 3}, {4, 2}}));
}

TEST(ClusterTest, GroupStraddlingZeroIsOneGroup) {
  // 359.5, 0.2 and 0.9 are one group; 180 another. The straddling group starts after its gap.
  EXPECT_EQ(cluster_deg({0.2, 180.0, 359.5, 0.9, -0.3}, 2.0), (Groups{{1}, {2, 4, 0, 3}}));
}

TEST(ClusterTest, NoGapAboveTheThresholdGivesOneGroupFromTheSmallestAngle) {
  EXPECT_EQ(cluster_deg({90.0, 0.0, 270.0, 180.0}, 100.0), (Groups{{1, 0, 3, 2}}));
}

TEST(ClusterTest, EqualAnglesFollowTheTieKeys) {
  EXPECT_EQ(cluster_deg({5.0, 5.0, 5.0}, 1.0, {30, 10, 20}), (Groups{{1, 2, 0}}));
}

TEST(ClusterTest, EmptyAndSingle) {
  EXPECT_TRUE(cluster_deg({}, 1.0).empty());
  EXPECT_EQ(cluster_deg({42.0}, 1.0), (Groups{{0}}));
}

TEST(ClusterTest, GapJustBelowTheThresholdDoesNotSplit) {
  EXPECT_EQ(cluster_deg({0.0, 1.0, 2.0}, 1.0 + 1e-9).size(), 1U);
}

TEST(LaunchTest, ParsesInternationalDesignators) {
  EXPECT_EQ(launch_of("2021-044A"), (Launch{.year = 2021, .number = 44}));
  EXPECT_EQ(launch_of("2019-074BZ"), (Launch{.year = 2019, .number = 74}));
  EXPECT_EQ(launch_of("2026-204ABC"), (Launch{.year = 2026, .number = 204}));
  for (const char* bad : {"", "2021-044", "2021-44A", "2021-044ABCD", "2021-044a", "2021_044A",
                          "20X1-044A", "2021-0X4A", " 2021-044A"}) {
    EXPECT_FALSE(launch_of(bad).has_value()) << bad;
  }
  // A failed parse falls back to a value that makes the comparison fail.
  constexpr Launch kNever{.year = 9999, .number = 999};
  EXPECT_LT(launch_of("2021-044Z").value_or(kNever), launch_of("2021-045A").value_or(Launch{}));
  EXPECT_LT(launch_of("2020-999A").value_or(kNever), launch_of("2021-001A").value_or(Launch{}));
}

// SGP4 positions of an ideal satellite give back its elements: inclination and RAAN to within
// SGP4's short-period terms, the RAAN drift close to the J2 formula.
TEST(MeasureGeometryTest, SyntheticSatelliteGivesBackItsElements) {
  WalkerShell shell;
  shell.inclination_deg = 53.0;
  shell.height_km = 550.0;
  shell.first_raan_deg = 123.0;
  const ElementSet e = walker_element_sets(shell).front();
  const auto built = Propagator::create(e);
  ASSERT_TRUE(std::holds_alternative<Propagator>(built));
  const double period_s = 86400.0 / e.mean_motion_rev_per_day;
  const auto measured =
      measure_orbit_geometry(std::get<Propagator>(built), utc(e.epoch), period_s, {});
  ASSERT_TRUE(std::holds_alternative<OrbitGeometry>(measured));
  const auto& g = std::get<OrbitGeometry>(measured);

  EXPECT_NEAR(rad_to_deg(g.inclination_rad), 53.0, 0.02);
  EXPECT_NEAR(rad_to_deg(g.raan_teme_rad), 123.0, 0.02);
  // Mean anomaly 0 at epoch, argument of perigee 0: argument of latitude 0.
  EXPECT_NEAR(rad_to_deg(wrap_pi(g.argument_of_latitude_rad)), 0.0, 0.05);
  EXPECT_NEAR(g.mean_normal_teme.norm(), 1.0, kExact);
  // The mean radius is close to the semi-major axis 6928.135 km (SGP4 reads the mean motion as
  // Kozai's, and adds J2 terms: a few km). The geodetic height is measured from an ellipsoid
  // whose radius under the track lies between the polar and equatorial radii.
  EXPECT_NEAR(g.mean_radius_km, 6928.135, 10.0);
  EXPECT_GT(g.mean_radius_km - g.mean_height_km, 6356.752);
  EXPECT_LT(g.mean_radius_km - g.mean_height_km, 6378.137);

  // J2 nodal regression -1.5 n J2 (R/a)^2 cos i for a circular orbit, WGS-72.
  const double a = 6378.135 + 550.0;
  const double n_rad_per_day = e.mean_motion_rev_per_day * 2 * kPi;
  const double expected =
      -1.5 * n_rad_per_day * 0.001082616 * std::pow(6378.135 / a, 2) * std::cos(e.inclination_rad);
  if (!g.raan_drift_rad_per_day) {
    FAIL() << "no drift";
  }
  EXPECT_NEAR(*g.raan_drift_rad_per_day / expected, 1.0, 0.01);

  // Osculating values at the reference time differ from the means by short-period terms only.
  EXPECT_NEAR(rad_to_deg(g.osculating_inclination_rad), 53.0, 0.05);
  EXPECT_NEAR(rad_to_deg(wrap_pi(g.osculating_raan_teme_rad - g.raan_teme_rad)), 0.0, 0.05);
}

// The argument of latitude is the one at the reference time, not at the start of the window.
TEST(MeasureGeometryTest, ArgumentOfLatitudeAdvancesWithTheReferenceTime) {
  WalkerShell shell;
  const ElementSet e = walker_element_sets(shell).front();
  const Propagator p = std::get<Propagator>(Propagator::create(e));
  const double period_s = 86400.0 / e.mean_motion_rev_per_day;
  const auto quarter = std::get<OrbitGeometry>(
      measure_orbit_geometry(p, utc(e.epoch).plus_seconds(period_s / 4), period_s, {}));
  EXPECT_NEAR(rad_to_deg(quarter.argument_of_latitude_rad), 90.0, 0.1);
}

// Vallado's case 28872 decays between 50 and 55 minutes after epoch.
ElementSet decaying() {
  ElementSet e;
  e.norad_cat_id = 28872;
  e.object_id = "2005-048B";
  e.epoch = CivilTime{2026, 11, 17, 18, 0, 0.0};
  e.mean_motion_rev_per_day = 16.46015938;
  e.eccentricity = 0.0303955;
  e.inclination_rad = deg_to_rad(96.4736);
  e.raan_rad = deg_to_rad(157.9986);
  e.arg_perigee_rad = deg_to_rad(244.0492);
  e.mean_anomaly_rad = deg_to_rad(110.6523);
  e.bstar_per_earth_radius = 0.00024476;
  return e;
}

TEST(MeasureGeometryTest, DecayInTheWindowIsAnError) {
  const ElementSet e = decaying();
  const Propagator p = std::get<Propagator>(Propagator::create(e));
  const double period_s = 86400.0 / e.mean_motion_rev_per_day;
  const auto measured = measure_orbit_geometry(p, utc(e.epoch).plus_seconds(3600.0), period_s, {});
  ASSERT_TRUE(std::holds_alternative<PropagationError>(measured));
  EXPECT_EQ(std::get<PropagationError>(measured), PropagationError::kDecayed);
}

// A circular orbit near 270 km with heavy drag: SGP4 (python-sgp4 2.27, same code) reports it
// decayed about 1145 minutes after epoch. So the window around the epoch propagates and the
// drift window a day later does not, which only leaves the drift empty.
TEST(MeasureGeometryTest, DecayBeforeTheDriftWindowLeavesOnlyTheDriftEmpty) {
  WalkerShell shell;
  ElementSet e = walker_element_sets(shell).front();
  e.mean_motion_rev_per_day = 16.2;
  e.bstar_per_earth_radius = 0.025;
  const Propagator p = std::get<Propagator>(Propagator::create(e));
  const double period_s = 86400.0 / e.mean_motion_rev_per_day;
  EXPECT_TRUE(
      std::holds_alternative<PropagationError>(p.propagate(utc(e.epoch).plus_seconds(86400.0))));
  const auto measured = measure_orbit_geometry(p, utc(e.epoch), period_s, {});
  ASSERT_TRUE(std::holds_alternative<OrbitGeometry>(measured));
  EXPECT_FALSE(std::get<OrbitGeometry>(measured).raan_drift_rad_per_day.has_value());
  EXPECT_NEAR(rad_to_deg(std::get<OrbitGeometry>(measured).inclination_rad), 53.0, 0.05);
}

// Averaging over an even sampling of one orbit: 4 samples already give the same plane to
// within the short-period terms.
TEST(MeasureGeometryTest, FewSamplesGiveTheSamePlane) {
  WalkerShell shell;
  const ElementSet e = walker_element_sets(shell).front();
  const Propagator p = std::get<Propagator>(Propagator::create(e));
  const double period_s = 86400.0 / e.mean_motion_rev_per_day;
  const auto many = std::get<OrbitGeometry>(measure_orbit_geometry(p, utc(e.epoch), period_s, {}));
  const auto few = std::get<OrbitGeometry>(
      measure_orbit_geometry(p, utc(e.epoch), period_s, GeometrySampling{.samples_per_orbit = 4}));
  EXPECT_NEAR(rad_to_deg(wrap_pi(many.raan_teme_rad - few.raan_teme_rad)), 0.0, 0.01);
  EXPECT_NEAR(rad_to_deg(many.inclination_rad - few.inclination_rad), 0.0, 0.01);
}

}  // namespace
