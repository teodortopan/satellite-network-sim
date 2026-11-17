#include "satsim/frames/teme_ecef.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>

#include "satsim/time/gmst.hpp"
#include "satsim/time/utc_time.hpp"

namespace {

using satsim::frames::teme_to_ecef;
using satsim::time::gmst_rad;
using satsim::time::UtcTime;

constexpr double kPi = std::numbers::pi;
constexpr double kArcsecondRad = kPi / (180.0 * 3600.0);

UtcTime at(int year, int month, int day, int hour, int minute, double second) {
  const std::optional<UtcTime> t = UtcTime::from_civil({year, month, day, hour, minute, second});
  if (!t.has_value()) {
    // A test fixture names a date that does not exist: the test itself is wrong.
    ADD_FAILURE() << "invalid fixture date " << year << "-" << month << "-" << day;
    std::abort();
  }
  return *t;
}

void expect_vector_near(const Eigen::Vector3d& actual, const Eigen::Vector3d& expected,
                        double tolerance_km) {
  EXPECT_NEAR(actual.x(), expected.x(), tolerance_km);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance_km);
  EXPECT_NEAR(actual.z(), expected.z(), tolerance_km);
  EXPECT_LE((actual - expected).norm(), tolerance_km);
}

// ---------------------------------------------------------------------------------------
// Vallado's TEME example: Vallado, Crawford, Hujsak & Kelso, "Revisiting Spacetrack Report
// #3", AIAA 2006-6753 Rev 2, Appendix C (read from the CelesTrak PDF on 2026-11-17):
//   2004 April 6, 07:51:28.386 UTC, dUT1 = -0.439961 s, xp = -0.140682", yp = 0.333309"
//   r_TEME = 5094.18016210  6127.64465950  6380.34453270 km
//   r_ITRF = -1033.47938300 7901.29527540  6380.35659580 km
// The paper does not print r_PEF. Fundamentals of Astrodynamics (4th ed.) Example 3-15 uses
// the same case with the epoch 07:51:28.386009 and dUT1 = -0.4399619 s (UT1 8.1 us later
// than the paper's inputs: 5.9e-10 rad, 4.7 mm here) and is quoted as
// r_PEF = -1033.4750313 7901.3055856 6380.3445328 km. I could not open the book; that r_PEF
// is reproduced to 0.1 mm by re-evaluating Vallado's jday + gstime (which carry the UT1
// Julian date as ONE double) in Python, so it is used, with the book's inputs.
//
// Vallado's single-double Julian date resolves 40 us (ULP of 2.45e6 d), which shifts his
// GMST by up to 2.9e-9 rad, 23 mm at this 7968 km equatorial radius. In this example our
// two-part evaluation (like pyerfa's) sits 8.5 mm from his PEF with the book's inputs and
// 13 mm from his ITRF with the paper's. kValladoToleranceKm = 25 mm covers one full ULP of
// his JD; the tight check is against pyerfa (same model, two-part date).
// ---------------------------------------------------------------------------------------

const Eigen::Vector3d kExampleTemeKm{5094.18016210, 6127.64465950, 6380.34453270};
constexpr double kValladoToleranceKm = 25e-6;

// Same tolerance argument as GMST: pyerfa and gmst_rad agree to ~1e-12 rad; the test allows
// 1e-10 rad, which at |r| = 10208 km is 1e-6 km. The rotation itself adds only ULP-level
// error (1e-12 km).
constexpr double kModelToleranceKm = 1e-6;

// UT1 instants built by applying the published UT1 - UTC to the published UTC epoch.
UtcTime paper_ut1() { return at(2004, 4, 6, 7, 51, 28.386).plus_seconds(-0.439961); }
UtcTime book_ut1() { return at(2004, 4, 6, 7, 51, 28.386009).plus_seconds(-0.4399619); }

// Polar motion W = ROT2(-xp) ROT1(-yp) takes PEF to ITRF (Vallado eq. C-2, written with
// the transpose convention that reproduces the paper's r_ITRF). Test-only: the library
// deliberately stops at PEF.
Eigen::Vector3d pef_to_itrf(const Eigen::Vector3d& pef, double xp_rad, double yp_rad) {
  Eigen::Matrix3d rot1_minus_yp;
  rot1_minus_yp << 1.0, 0.0, 0.0, 0.0, std::cos(yp_rad), -std::sin(yp_rad), 0.0, std::sin(yp_rad),
      std::cos(yp_rad);
  Eigen::Matrix3d rot2_minus_xp;
  rot2_minus_xp << std::cos(xp_rad), 0.0, std::sin(xp_rad), 0.0, 1.0, 0.0, -std::sin(xp_rad), 0.0,
      std::cos(xp_rad);
  return rot2_minus_xp * rot1_minus_yp * pef;
}

TEST(TemeEcefTest, ValladoExamplePef) {
  const Eigen::Vector3d pef = teme_to_ecef(kExampleTemeKm, book_ut1());
  expect_vector_near(pef, {-1033.4750313, 7901.3055856, 6380.3445328}, kValladoToleranceKm);
}

// pyerfa 2.0.1.5: g = erfa.gmst82(2453101.5, (7*3600 + 51*60 + 28.386 - 0.439961) / 86400)
// = 5.4595625860266885 rad; r_PEF = R3(g) r_TEME computed with numpy 2.5.5.
TEST(TemeEcefTest, ExampleMatchesErfaGmst82) {
  const Eigen::Vector3d pef = teme_to_ecef(kExampleTemeKm, paper_ut1());
  expect_vector_near(pef, {-1033.4750444881, 7901.3055838546, 6380.3445327000}, kModelToleranceKm);
}

// End-to-end against the vector the paper actually prints: applying the published polar
// motion to our PEF must give the published ITRF.
TEST(TemeEcefTest, ValladoExampleItrfWithPolarMotion) {
  const Eigen::Vector3d pef = teme_to_ecef(kExampleTemeKm, paper_ut1());
  const Eigen::Vector3d itrf =
      pef_to_itrf(pef, -0.140682 * kArcsecondRad, 0.333309 * kArcsecondRad);
  expect_vector_near(itrf, {-1033.47938300, 7901.29527540, 6380.35659580}, kValladoToleranceKm);
  // And the size of what PEF neglects, as documented on teme_to_ecef: ~16 m here.
  EXPECT_GT((itrf - pef).norm(), 0.010);
  EXPECT_LT((itrf - pef).norm(), 0.020);
}

// A rotation about z preserves length and z exactly in exact arithmetic. cos and sin are
// each within 1 ULP, so |r| changes by a few ULPs relative: allow 1e-14 relative.
TEST(TemeEcefTest, PreservesLengthAndZ) {
  const std::array<Eigen::Vector3d, 4> vectors{kExampleTemeKm, Eigen::Vector3d{-6928.137, 0.0, 0.0},
                                               Eigen::Vector3d{1.0, -2.0, 3.0},
                                               Eigen::Vector3d{0.0, 42164.0, -10.0}};
  for (const UtcTime t : {paper_ut1(), at(2026, 11, 17, 18, 30, 0.0)}) {
    for (const Eigen::Vector3d& r : vectors) {
      const Eigen::Vector3d e = teme_to_ecef(r, t);
      EXPECT_EQ(e.z(), r.z());
      EXPECT_NEAR(e.norm(), r.norm(), 1e-14 * r.norm());
    }
  }
}

// A point on the TEME x-axis (the equinox direction) lies at Earth-fixed longitude -GMST:
// Greenwich is GMST east of the equinox. The angle is recovered by atan2 from rounded
// cos/sin, so within a few ULPs of pi: allow 1e-14 rad.
TEST(TemeEcefTest, EquinoxDirectionLandsAtMinusGmst) {
  for (const UtcTime t : {paper_ut1(), at(1992, 8, 20, 12, 14, 0.0), at(2026, 11, 17, 0, 0, 0.0)}) {
    const Eigen::Vector3d e = teme_to_ecef({7000.0, 0.0, 0.0}, t);
    const double longitude = std::atan2(e.y(), e.x());
    EXPECT_NEAR(std::remainder(longitude + gmst_rad(t), 2.0 * kPi), 0.0, 1e-14);
    EXPECT_EQ(e.z(), 0.0);
  }
}

}  // namespace
