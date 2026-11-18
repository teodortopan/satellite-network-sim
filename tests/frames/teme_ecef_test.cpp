#include "satsim/frames/teme_ecef.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>

#include "erfa_reference.hpp"
#include "satsim/time/earth_orientation.hpp"
#include "satsim/time/gmst.hpp"
#include "satsim/time/time_scales.hpp"
#include "satsim/time/utc_time.hpp"

namespace {

using satsim::frames::pef_to_itrf;
using satsim::frames::teme_to_ecef;
using satsim::frames::teme_to_pef;
using satsim::time::EarthOrientation;
using satsim::time::gmst_rad;
using satsim::time::ut1_from_utc;
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

Eigen::Vector3d to_eigen(const std::array<double, 3>& v) { return {v[0], v[1], v[2]}; }

void expect_vector_near(const Eigen::Vector3d& actual, const Eigen::Vector3d& expected,
                        double tolerance_km) {
  EXPECT_NEAR(actual.x(), expected.x(), tolerance_km);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance_km);
  EXPECT_NEAR(actual.z(), expected.z(), tolerance_km);
  EXPECT_LE((actual - expected).norm(), tolerance_km);
}

// ---------------------------------------------------------------------------------------
// Vallado's TEME example: Vallado, Crawford, Hujsak & Kelso, "Revisiting Spacetrack Report
// #3", AIAA 2006-6753 Rev 2, Appendix C (read from the CelesTrak PDF on 2026-11-17 and
// again on 2026-11-18):
//   2004 April 6, 07:51:28.386 UTC, dUT1 = -0.439961 s, xp = -0.140682", yp = 0.333309"
//   r_TEME = 5094.18016210  6127.64465950  6380.34453270 km
//   r_ITRF = -1033.47938300 7901.29527540  6380.35659580 km
// The paper does not print r_PEF. Fundamentals of Astrodynamics (4th ed.) Example 3-15 uses
// the same case with the epoch 07:51:28.386009 and dUT1 = -0.4399619 s (UT1 8.1 us later
// than the paper's inputs: 5.9e-10 rad, 4.7 mm here) and is quoted as
// r_PEF = -1033.4750313 7901.3055856 6380.3445328 km. I could not open the book; that r_PEF
// is reproduced to 0.1 mm by re-evaluating Vallado's jday + gstime (which carry the UT1
// Julian date as ONE double) in Python, so it is used, with the book's inputs. The book's
// own r_ITRF and its J2000 / GCRF vectors are not used: no copy was available to quote.
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
// 1e-10 rad, which at |r| = 10208 km is 1e-6 km. The rotations themselves add only
// ULP-level error (1e-12 km), and the order of the two polar-motion rotations (ERFA's
// pom00 applies them in the IERS order) changes the result by x_p * y_p * |r|, 1e-8 km.
constexpr double kModelToleranceKm = 1e-6;

UtcTime paper_utc() { return at(2004, 4, 6, 7, 51, 28.386); }
const EarthOrientation kPaperEarthOrientation{.ut1_minus_utc_s = -0.439961,
                                              .polar_x_rad = -0.140682 * kArcsecondRad,
                                              .polar_y_rad = 0.333309 * kArcsecondRad};
UtcTime book_ut1() { return at(2004, 4, 6, 7, 51, 28.386009).plus_seconds(-0.4399619); }

TEST(TemeEcefTest, ValladoExamplePef) {
  const Eigen::Vector3d pef = teme_to_pef(kExampleTemeKm, book_ut1());
  expect_vector_near(pef, {-1033.4750313, 7901.3055856, 6380.3445328}, kValladoToleranceKm);
}

// End to end against the vector the paper prints: UTC epoch plus its Earth orientation.
TEST(TemeEcefTest, ValladoExampleItrf) {
  const Eigen::Vector3d itrf = teme_to_ecef(kExampleTemeKm, paper_utc(), kPaperEarthOrientation);
  expect_vector_near(itrf, {-1033.47938300, 7901.29527540, 6380.35659580}, kValladoToleranceKm);
}

// The polar-motion signs matter: each flipped sign moves this position by about 10 m, so
// the published r_ITRF rejects every wrong combination by far more than the tolerance.
TEST(TemeEcefTest, ValladoExampleRejectsWrongPolarMotionSigns) {
  const Eigen::Vector3d published{-1033.47938300, 7901.29527540, 6380.35659580};
  const Eigen::Vector3d pef = teme_to_pef(
      kExampleTemeKm, ut1_from_utc(paper_utc(), kPaperEarthOrientation.ut1_minus_utc_s));
  const double xp = kPaperEarthOrientation.polar_x_rad;
  const double yp = kPaperEarthOrientation.polar_y_rad;
  EXPECT_LT((pef_to_itrf(pef, xp, yp) - published).norm(), kValladoToleranceKm);
  EXPECT_GT((pef_to_itrf(pef, -xp, yp) - published).norm(), 0.004);
  EXPECT_GT((pef_to_itrf(pef, xp, -yp) - published).norm(), 0.004);
  EXPECT_GT((pef_to_itrf(pef, -xp, -yp) - published).norm(), 0.004);
  // And skipping polar motion altogether is off by ~16 m.
  EXPECT_GT((pef - published).norm(), 0.010);
}

// ---------------------------------------------------------------------------------------
// Against ERFA (tests/frames/erfa_reference.hpp, generated by python/gen_frame_reference.py
// with pyerfa): GMST from erfa.gmst82 at UT1, PEF = R3(gmst) r_TEME, ITRF = erfa.pom00 r_PEF.
// ---------------------------------------------------------------------------------------

TEST(TemeEcefTest, MatchesErfa) {
  namespace ref = satsim_test::erfa_reference;
  for (const ref::EcefCase& c : ref::kEcefCases) {
    SCOPED_TRACE(testing::Message() << c.utc.year << "-" << c.utc.month << "-" << c.utc.day);
    const UtcTime utc =
        at(c.utc.year, c.utc.month, c.utc.day, c.utc.hour, c.utc.minute, c.utc.second);
    const UtcTime ut1 = ut1_from_utc(utc, c.ut1_minus_utc_s);
    EXPECT_NEAR(std::remainder(gmst_rad(ut1) - c.gmst_rad, 2.0 * kPi), 0.0, 1e-10);
    const Eigen::Vector3d teme = to_eigen(ref::kTemePositionKm);
    expect_vector_near(teme_to_pef(teme, ut1), to_eigen(c.pef_km), kModelToleranceKm);
    const EarthOrientation eop{.ut1_minus_utc_s = c.ut1_minus_utc_s,
                               .polar_x_rad = c.polar_x_arcsec * kArcsecondRad,
                               .polar_y_rad = c.polar_y_arcsec * kArcsecondRad};
    expect_vector_near(teme_to_ecef(teme, utc, eop), to_eigen(c.itrf_km), kModelToleranceKm);
  }
}

// A zero sample is exactly the GMST rotation at UTC: UT1 = UTC + 0, and a rotation by zero
// has cos = 1 and sin = 0 exactly.
TEST(TemeEcefTest, ZeroEarthOrientationEqualsPef) {
  for (const UtcTime t : {paper_utc(), at(2026, 11, 18, 13, 7, 41.25)}) {
    for (const Eigen::Vector3d& r : {kExampleTemeKm, Eigen::Vector3d{-6928.137, 1.0, -2.0},
                                     Eigen::Vector3d{0.0, 0.0, 7000.0}}) {
      EXPECT_EQ(teme_to_ecef(r, t, EarthOrientation{}), teme_to_pef(r, t));
    }
  }
}

// ---------------------------------------------------------------------------------------
// The size of each correction at Starlink radius (6928.137 km, 550 km altitude), with the
// finals2000A values of 2026-11-17 0h UTC: UT1 - UTC = -0.0714893 s, x_p = 0.118407",
// y_p = 0.326797".
// ---------------------------------------------------------------------------------------

constexpr double kStarlinkRadiusKm = 6928.137;
const EarthOrientation k20261117{.ut1_minus_utc_s = -0.0714893,
                                 .polar_x_rad = 0.118407 * kArcsecondRad,
                                 .polar_y_rad = 0.326797 * kArcsecondRad};

// UT1 - UTC turns GMST by its rate times dUT1. GMST advances 360.985647366 deg per day of
// UT1 near J2000 (IAU 1982; see gmst_test.cpp), 7.29211585e-5 rad/s, so -0.0714893 s is
// 5.21e-6 rad and an equatorial point moves 36.117 m (chord and arc agree to 1e-12
// relative). Tolerance 1 um: the rate in 2026 differs by 6e-9 relative (2e-10 m here) and
// the rotations round at 1e-12 km.
TEST(TemeEcefTest, SizeOfUt1CorrectionAtStarlinkRadius) {
  const UtcTime t = at(2026, 11, 17, 0, 0, 0.0);
  const Eigen::Vector3d r{kStarlinkRadiusKm * std::cos(0.3), kStarlinkRadiusKm * std::sin(0.3),
                          0.0};
  const EarthOrientation ut1_only{.ut1_minus_utc_s = k20261117.ut1_minus_utc_s};
  const double shift_km = (teme_to_ecef(r, t, ut1_only) - teme_to_pef(r, t)).norm();
  const double gmst_rate_rad_per_s = 360.985647366 * kPi / 180.0 / 86400.0;
  const double expected_km = gmst_rate_rad_per_s * 0.0714893 * kStarlinkRadiusKm;
  EXPECT_NEAR(expected_km, 0.036117, 1e-6);
  EXPECT_NEAR(shift_km, expected_km, 1e-6);
}

// Polar motion tilts the frame by sqrt(x_p^2 + y_p^2) = 0.347585" = 1.6851e-6 rad (to first
// order), which moves a point on the rotation axis by 11.675 m; no point at this radius
// moves more (1 mm margin for second-order terms of 1e-12 relative).
TEST(TemeEcefTest, SizeOfPolarMotionAtStarlinkRadius) {
  const double tilt_rad = std::hypot(k20261117.polar_x_rad, k20261117.polar_y_rad);
  const double expected_km = tilt_rad * kStarlinkRadiusKm;
  EXPECT_NEAR(expected_km, 0.011675, 1e-6);
  const Eigen::Vector3d pole{0.0, 0.0, kStarlinkRadiusKm};
  EXPECT_NEAR((pef_to_itrf(pole, k20261117.polar_x_rad, k20261117.polar_y_rad) - pole).norm(),
              expected_km, 1e-6);
  for (int i = 0; i < 36; ++i) {
    const double longitude = 2.0 * kPi * static_cast<double>(i) / 36.0;
    for (const double latitude : {-1.2, -0.4, 0.0, 0.5, 1.4}) {
      const Eigen::Vector3d r =
          kStarlinkRadiusKm * Eigen::Vector3d{std::cos(latitude) * std::cos(longitude),
                                              std::cos(latitude) * std::sin(longitude),
                                              std::sin(latitude)};
      const double shift_km =
          (pef_to_itrf(r, k20261117.polar_x_rad, k20261117.polar_y_rad) - r).norm();
      EXPECT_LE(shift_km, expected_km + 1e-6);
    }
  }
}

// ---------------------------------------------------------------------------------------
// Properties of the GMST rotation (TEME to PEF).
// ---------------------------------------------------------------------------------------

// A rotation about z preserves length and z exactly in exact arithmetic. cos and sin are
// each within 1 ULP, so |r| changes by a few ULPs relative: allow 1e-14 relative.
TEST(TemeEcefTest, PefPreservesLengthAndZ) {
  const std::array<Eigen::Vector3d, 4> vectors{kExampleTemeKm, Eigen::Vector3d{-6928.137, 0.0, 0.0},
                                               Eigen::Vector3d{1.0, -2.0, 3.0},
                                               Eigen::Vector3d{0.0, 42164.0, -10.0}};
  for (const UtcTime t : {book_ut1(), at(2026, 11, 17, 18, 30, 0.0)}) {
    for (const Eigen::Vector3d& r : vectors) {
      const Eigen::Vector3d e = teme_to_pef(r, t);
      EXPECT_EQ(e.z(), r.z());
      EXPECT_NEAR(e.norm(), r.norm(), 1e-14 * r.norm());
      EXPECT_NEAR(teme_to_ecef(r, t, k20261117).norm(), r.norm(), 1e-14 * r.norm());
    }
  }
}

// A point on the TEME x-axis (the equinox direction) lies at Earth-fixed longitude -GMST:
// Greenwich is GMST east of the equinox. The angle is recovered by atan2 from rounded
// cos/sin, so within a few ULPs of pi: allow 1e-14 rad.
TEST(TemeEcefTest, EquinoxDirectionLandsAtMinusGmst) {
  for (const UtcTime t : {book_ut1(), at(1992, 8, 20, 12, 14, 0.0), at(2026, 11, 17, 0, 0, 0.0)}) {
    const Eigen::Vector3d e = teme_to_pef({7000.0, 0.0, 0.0}, t);
    const double longitude = std::atan2(e.y(), e.x());
    EXPECT_NEAR(std::remainder(longitude + gmst_rad(t), 2.0 * kPi), 0.0, 1e-14);
    EXPECT_EQ(e.z(), 0.0);
  }
}

}  // namespace
