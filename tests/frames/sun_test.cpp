#include "satsim/frames/sun.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>

#include "satsim/time/civil_time.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/time_scales.hpp"
#include "satsim/time/utc_time.hpp"
#include "sun_reference.hpp"

namespace {

using satsim::frames::sun_direction_ecef;
using satsim::frames::sun_direction_mod;
using satsim::frames::sun_direction_teme;
using satsim::time::CivilTime;
using satsim::time::EarthOrientation;
using satsim::time::UtcTime;

namespace ref = satsim_test::sun_reference;

constexpr double kRadPerDeg = std::numbers::pi / 180.0;
constexpr double kArcsecondRad = kRadPerDeg / 3600.0;

// The Almanac states 0.01 deg for 1950-2050; against ERFA the formula's error is at most
// 0.0106 deg over that range (0.011 deg in 2026). 0.02 deg leaves room for that and still
// catches any real mistake: the wrong frame (precession since J2000 is 0.37 deg by 2026,
// nutation 0.005 deg is the only thing smaller), the wrong sign of an axis, or a day's error
// in the time argument (1 deg).
constexpr double kToleranceRad = 0.02 * kRadPerDeg;

UtcTime at(const CivilTime& c) {
  const std::optional<UtcTime> t = UtcTime::from_civil(c);
  if (!t.has_value()) {
    ADD_FAILURE() << "invalid fixture date";
    std::abort();
  }
  return *t;
}

double angle_between(const Eigen::Vector3d& a, const std::array<double, 3>& b) {
  const Eigen::Vector3d other(b[0], b[1], b[2]);
  return std::atan2(a.cross(other).norm(), a.dot(other));
}

TEST(SunTest, ModDirectionMatchesErfa) {
  for (const ref::SunCase& c : ref::kSunCases) {
    const Eigen::Vector3d sun = sun_direction_mod(at(c.tt));
    EXPECT_NEAR(sun.norm(), 1.0, 1e-15);
    EXPECT_LE(angle_between(sun, c.mod), kToleranceRad) << c.tt.year << "-" << c.tt.month;
  }
}

TEST(SunTest, TemeDirectionMatchesErfa) {
  for (const ref::SunCase& c : ref::kSunCases) {
    const Eigen::Vector3d sun = sun_direction_teme(at(c.tt));
    EXPECT_NEAR(sun.norm(), 1.0, 1e-15);
    EXPECT_LE(angle_between(sun, c.teme), kToleranceRad) << c.tt.year << "-" << c.tt.month;
  }
}

// The frame step itself (MOD to TEME) is exact: the rotation ERFA applied to its own MOD
// vector maps the formula's MOD vector to the formula's TEME vector, so the angle between the
// formula and ERFA is the same in both frames to well under a microradian.
TEST(SunTest, ModToTemeRotationAddsNoError) {
  for (const ref::SunCase& c : ref::kSunCases) {
    const UtcTime tt = at(c.tt);
    const double mod_error = angle_between(sun_direction_mod(tt), c.mod);
    const double teme_error = angle_between(sun_direction_teme(tt), c.teme);
    EXPECT_NEAR(mod_error, teme_error, 1e-7);
  }
}

TEST(SunTest, EcefDirectionMatchesErfa) {
  for (const ref::SunEcefCase& c : ref::kSunEcefCases) {
    const EarthOrientation eop{.ut1_minus_utc_s = c.ut1_minus_utc_s,
                               .polar_x_rad = c.polar_x_arcsec * kArcsecondRad,
                               .polar_y_rad = c.polar_y_arcsec * kArcsecondRad};
    const Eigen::Vector3d sun = sun_direction_ecef(at(c.utc), eop);
    EXPECT_NEAR(sun.norm(), 1.0, 1e-15);
    EXPECT_LE(angle_between(sun, c.itrf), kToleranceRad) << c.utc.hour;
  }
}

// Plain facts any formula must reproduce: in the equatorial frame the Sun's declination is
// about +23.44 deg at the June solstice and about 0 at the March equinox, and in the
// Earth-fixed frame it is over the Greenwich meridian (x > 0, y about 0) near 12h UTC.
TEST(SunTest, SolsticeEquinoxAndNoon) {
  const Eigen::Vector3d june = sun_direction_mod(at({2026, 6, 21, 8, 24, 0.0}));
  EXPECT_NEAR(std::asin(june.z()) / kRadPerDeg, 23.44, 0.01);
  const Eigen::Vector3d march = sun_direction_mod(at({2026, 3, 20, 14, 46, 0.0}));
  EXPECT_NEAR(std::asin(march.z()) / kRadPerDeg, 0.0, 0.02);
  EXPECT_GT(march.x(), 0.9999);  // At the vernal equinox the Sun is on the x axis.

  // Near the March equinox the equation of time is about -7.5 minutes, so the Sun crosses
  // Greenwich at about 12:07.5 UTC.
  const Eigen::Vector3d noon = sun_direction_ecef(at({2026, 3, 20, 12, 7, 30.0}), {});
  EXPECT_GT(noon.x(), 0.999);
  EXPECT_NEAR(std::atan2(noon.y(), noon.x()) / kRadPerDeg, 0.0, 0.2);
}

}  // namespace
