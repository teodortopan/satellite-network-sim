#include "satsim/time/gmst.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>

#include "satsim/core/angles.hpp"
#include "satsim/time/utc_time.hpp"

namespace {

using satsim::rad_to_deg;
using satsim::time::gmst_rad;
using satsim::time::UtcTime;

constexpr double kTwoPi = 2.0 * std::numbers::pi;

UtcTime at(int year, int month, int day, int hour, int minute, double second) {
  const std::optional<UtcTime> t = UtcTime::from_civil({year, month, day, hour, minute, second});
  if (!t.has_value()) {
    // A test fixture names a date that does not exist: the test itself is wrong.
    ADD_FAILURE() << "invalid fixture date " << year << "-" << month << "-" << day;
    std::abort();
  }
  return *t;
}

// Signed difference of two angles, in (-pi, pi].
double angle_difference(double a, double b) { return std::remainder(a - b, kTwoPi); }

// Tolerance against an independent double-precision evaluation of the same IAU 1982
// formula (pyerfa). Both evaluate the polynomial from a two-part date; the largest rounding
// is in 8640184.812866 * T (ULP ~2e-9 s of time, 1.4e-13 rad), so agreement is expected at
// ~1e-12 rad. 1e-10 rad leaves margin for arm64/x86-64 libm differences; it is 1.4 us of
// Earth rotation, 0.7 mm at a 6928 km orbit radius.
constexpr double kModelToleranceRad = 1e-10;

// IAU 1982 defines GMST at J2000.0 (2000 January 1, 12h UT1) as 18h 41m 50.54841s, the
// constant term of the formula (Aoki et al. 1982, A&A 105, 359; McCarthy 1992 / IERS
// Standards; Vallado, Fundamentals 4th ed., eq. 3-47). 67310.54841 s / 240 = 280.460618375
// degrees exactly. T = 0, so only the conversion and wrap round: a few ULPs of 280.
TEST(GmstTest, DefinitionAtJ2000) {
  const double gmst = gmst_rad(at(2000, 1, 1, 12, 0, 0.0));
  EXPECT_NEAR(rad_to_deg(gmst), 280.460618375, 1e-11);
  // pyerfa 2.0.1.5: erfa.gmst82(2451545.0, 0.0) = 4.894961212823059
  EXPECT_NEAR(gmst, 4.894961212823059, kModelToleranceRad);
}

// Vallado, "Fundamentals of Astrodynamics and Applications", Example 3-5: GMST for
// 1992 August 20, 12:14 UT1. I could not consult the book directly. Secondary sources
// reproduce the result as 152.5787879 deg (a GitHub PR for interstellar-tools quoting the
// example); Vallado's own single-double gstime gives 152.578787811 deg and the two-part
// evaluation gives 152.578787852 deg. All of these round to 152.578788 deg, which is
// checked here to half a unit in the 6th decimal; the model check below is the tight one.
TEST(GmstTest, ValladoExample3_5) {
  const double gmst = gmst_rad(at(1992, 8, 20, 12, 14, 0.0));
  EXPECT_NEAR(rad_to_deg(gmst), 152.578788, 5e-7);
  // pyerfa 2.0.1.5: erfa.gmst82(2448854.5, (12 + 14/60) / 24) = 2.663002216713352 rad
  //                 (152.578787851658 deg)
  EXPECT_NEAR(gmst, 2.663002216713352, kModelToleranceRad);
}

// More independent values across the 1957-2100 range. Each from pyerfa 2.0.1.5,
// erfa.gmst82(jd_whole, fraction), with the two-part UT1 date given.
TEST(GmstTest, MatchesErfaGmst82) {
  struct Case {
    UtcTime t;
    double expected_rad;
  };
  const std::array kCases{
      // erfa.gmst82(2436115.5, (19*3600 + 28*60 + 34) / 86400)
      Case{at(1957, 10, 4, 19, 28, 34.0), 5.329470619739467},
      // erfa.gmst82(2453101.5, 0.32740678285879626): Vallado's 2004 TEME example epoch,
      // 07:51:28.386 UTC with UT1 - UTC = -0.439961 s applied.
      Case{at(2004, 4, 6, 7, 51, 28.386).plus_seconds(-0.439961), 5.4595625860266885},
      // erfa.gmst82(2461361.5, 0.0)
      Case{at(2026, 11, 17, 0, 0, 0.0), 0.978571511238435},
      // erfa.gmst82(2488069.5, 0.0)
      Case{at(2100, 1, 1, 0, 0, 0.0), 1.758213904290670},
  };
  for (const Case& c : kCases) {
    EXPECT_NEAR(angle_difference(gmst_rad(c.t), c.expected_rad), 0.0, kModelToleranceRad)
        << "jd = " << c.t.jd_whole() << " + " << c.t.jd_fraction();
  }
}

// Over one solar day the Earth turns one full turn plus the sidereal excess. Starting at
// J2000.0 (T = 0) the excess is the linear IAU 1982 coefficient alone (Aoki et al. 1982):
// 8640184.812866 / 36525 s of time = 0.985647366286 deg, i.e. 360.985647366 deg per day.
// The T^2 term adds 0.093104 * (1/36525)^2 s, 5e-15 rad, which is negligible here.
// (pyerfa 2.0.1.5 agrees: erfa.gmst82(2451546.0, 0) - erfa.gmst82(2451545.0, 0) =
// 0.017202791805312145 rad.) Two evaluations each within ~1e-12 rad of exact.
TEST(GmstTest, SiderealRateAtJ2000) {
  const double expected_rad = 8640184.812866 / 36525.0 / 240.0 * std::numbers::pi / 180.0;
  const UtcTime t = at(2000, 1, 1, 12, 0, 0.0);
  const double advance = angle_difference(gmst_rad(t.plus_seconds(86400.0)), gmst_rad(t));
  EXPECT_NEAR(advance, expected_rad, kModelToleranceRad);
}

// Away from J2000 the T^2 term makes the daily advance grow: at T = 0.27 (2026) it adds
// 2 * 0.093104 * T / 36525 s = 1.4e-6 s of time, 1e-10 rad, so the T = 0 value no longer
// applies at this tolerance. Reference: pyerfa 2.0.1.5,
// erfa.gmst82(2461362.5, f) - erfa.gmst82(2461361.5, f) with f = (3*3600 + 14*60 + 15.9)/86400,
// wrapped to [0, 2*pi): 0.017202791904935566 rad.
TEST(GmstTest, SiderealRateIn2026) {
  const UtcTime t = at(2026, 11, 17, 3, 14, 15.9);
  const double advance = angle_difference(gmst_rad(t.plus_seconds(86400.0)), gmst_rad(t));
  EXPECT_NEAR(advance, 0.017202791904935566, kModelToleranceRad);
}

TEST(GmstTest, AlwaysInRange) {
  UtcTime t = at(1957, 1, 1, 0, 0, 0.0);
  const UtcTime end = at(2101, 1, 1, 0, 0, 0.0);
  while (t < end) {
    const double gmst = gmst_rad(t);
    ASSERT_GE(gmst, 0.0);
    ASSERT_LT(gmst, kTwoPi);
    t = t.plus_seconds(86400.0 * 7.0 + 3599.5);
  }
}

}  // namespace
