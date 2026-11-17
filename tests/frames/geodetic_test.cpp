#include "satsim/frames/geodetic.hpp"

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <array>
#include <cmath>
#include <numbers>

#include "satsim/core/angles.hpp"
#include "satsim/frames/wgs84.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::frames::ecef_to_geodetic;
using satsim::frames::Geodetic;
using satsim::frames::geodetic_to_ecef;
using satsim::frames::kWgs84EquatorialRadiusKm;
using satsim::frames::kWgs84PolarRadiusKm;

constexpr double kPi = std::numbers::pi;

// Tolerance against the references below. The references are printed to 1e-12 km, and
// pyerfa and pyproj agree on every forward value to < 1e-12 km. Our double arithmetic on
// magnitudes up to 4.2e4 km (ULP 7e-12 km) accumulates a few tens of ULPs, ~1e-10 km.
// 1e-8 km (10 um) is 100x that.
constexpr double kReferenceToleranceKm = 1e-8;

// The required round-trip accuracy: 1 mm.
constexpr double kRoundTripToleranceKm = 1e-6;

// Compares geodetic coordinates as distances: angular differences are scaled by the
// distance from the Earth's axis (longitude) or centre (latitude), so one tolerance in km
// applies at every height. Longitude is skipped on the polar axis, where it is undefined.
void expect_geodetic_near(const Geodetic& actual, const Geodetic& expected, double tolerance_km) {
  const double radius_km = kWgs84EquatorialRadiusKm + expected.height_km;
  EXPECT_LE(std::abs(actual.latitude_rad - expected.latitude_rad) * radius_km, tolerance_km)
      << "latitude " << actual.latitude_rad << " vs " << expected.latitude_rad;
  const double axis_distance_km = radius_km * std::cos(expected.latitude_rad);
  const double longitude_error_rad =
      std::remainder(actual.longitude_rad - expected.longitude_rad, 2.0 * kPi);
  EXPECT_LE(std::abs(longitude_error_rad) * axis_distance_km, tolerance_km)
      << "longitude " << actual.longitude_rad << " vs " << expected.longitude_rad;
  EXPECT_NEAR(actual.height_km, expected.height_km, tolerance_km);
}

void expect_vector_near(const Eigen::Vector3d& actual, const Eigen::Vector3d& expected,
                        double tolerance_km) {
  EXPECT_LE((actual - expected).norm(), tolerance_km)
      << "actual " << actual.transpose() << " expected " << expected.transpose();
}

Geodetic geodetic_deg(double latitude_deg, double longitude_deg, double height_km) {
  return {.latitude_rad = deg_to_rad(latitude_deg),
          .longitude_rad = deg_to_rad(longitude_deg),
          .height_km = height_km};
}

// ---------------------------------------------------------------------------------------
// Known points, both directions. ECEF values from pyerfa 2.0.1.5,
// erfa.gd2gc(1, lon_rad, lat_rad, h_m) (1 = WGS-84), converted to km, and confirmed by
// pyproj 3.8.0 / PROJ 9.8.1, Transformer.from_crs("EPSG:4979", "EPSG:4978") (difference
// < 1e-12 km on every point). The inverse direction recovers the geodetic input.
// ---------------------------------------------------------------------------------------

struct KnownPoint {
  const char* name;
  Geodetic geodetic;
  Eigen::Vector3d ecef_km;
};

const std::array kKnownPoints{
    // Exact by construction: (a, 0, 0) and (0, 0, b).
    KnownPoint{"equator, prime meridian", geodetic_deg(0.0, 0.0, 0.0), {6378.137, 0.0, 0.0}},
    KnownPoint{"north pole", geodetic_deg(90.0, 0.0, 0.0), {0.0, 0.0, 6356.752314245179}},
    KnownPoint{"Buenos Aires, 25 m",
               geodetic_deg(-34.603722, -58.381592, 0.025),
               {2755.265933382941, -4475.398439340587, -3601.782736806550}},
    KnownPoint{"Starlink shell, 550 km",
               geodetic_deg(53.0, 120.0, 550.0),
               {-2088.838995857464, 3617.975269656285, 5509.793033880422}},
    KnownPoint{"GEO, 75 W",
               geodetic_deg(0.0, -75.0, 35786.0),
               {10912.881675911864, -40727.428871490476, 0.0}},
    KnownPoint{"antimeridian, 1 km",
               geodetic_deg(10.0, 180.0, 1.0),
               {-6282.857637356466, 0.000000000001, 1100.422195913028}},
    KnownPoint{"near south pole, -10 km",
               geodetic_deg(-89.9, -170.0, -10.0),
               {-10.982515876984, -1.936513865171, -6346.742582340182}},
};

TEST(GeodeticTest, KnownPointsToEcef) {
  for (const KnownPoint& p : kKnownPoints) {
    SCOPED_TRACE(p.name);
    expect_vector_near(geodetic_to_ecef(p.geodetic), p.ecef_km, kReferenceToleranceKm);
  }
}

TEST(GeodeticTest, KnownPointsFromEcef) {
  for (const KnownPoint& p : kKnownPoints) {
    SCOPED_TRACE(p.name);
    expect_geodetic_near(ecef_to_geodetic(p.ecef_km), p.geodetic, kReferenceToleranceKm);
  }
}

// ECEF inputs with inverse references from pyerfa 2.0.1.5, erfa.gc2gd(1, xyz_m). erfa's
// own error on these, measured by mapping its result back with erfa.gd2gc, is 1.5e-10 km
// and 1.1e-12 km. (pyproj's inverse is NOT used as a reference: on the first point it is
// off by 1.4 mm, and by 98 mm at 32,676 km height.)
TEST(GeodeticTest, EcefPointsMatchErfaGc2gd) {
  expect_geodetic_near(ecef_to_geodetic({4000.0, -3000.0, 4500.0}),
                       geodetic_deg(42.168438083417129, -36.869897645844, 358.269715948824),
                       kReferenceToleranceKm);
  expect_geodetic_near(ecef_to_geodetic({-1000.0, -6000.0, -1500.0}),
                       geodetic_deg(-13.944010823650007, -99.462322208026, -111.923107528698),
                       kReferenceToleranceKm);
}

// ---------------------------------------------------------------------------------------
// Special points, exact by geometry.
// ---------------------------------------------------------------------------------------

TEST(GeodeticTest, PolesAndEquatorExactly) {
  for (const double height_km : {-10.0, 0.0, 550.0, 35786.0}) {
    const Geodetic north = ecef_to_geodetic({0.0, 0.0, kWgs84PolarRadiusKm + height_km});
    EXPECT_EQ(north.latitude_rad, kPi / 2.0);
    EXPECT_EQ(north.longitude_rad, 0.0);
    EXPECT_NEAR(north.height_km, height_km, kReferenceToleranceKm);

    const Geodetic south = ecef_to_geodetic({0.0, 0.0, -(kWgs84PolarRadiusKm + height_km)});
    EXPECT_EQ(south.latitude_rad, -kPi / 2.0);
    EXPECT_NEAR(south.height_km, height_km, kReferenceToleranceKm);

    const Geodetic equator = ecef_to_geodetic({0.0, kWgs84EquatorialRadiusKm + height_km, 0.0});
    EXPECT_EQ(equator.latitude_rad, 0.0);
    EXPECT_EQ(equator.longitude_rad, kPi / 2.0);
    EXPECT_NEAR(equator.height_km, height_km, kReferenceToleranceKm);
  }
}

// The documented behaviour at and near the Earth's centre.
TEST(GeodeticTest, EarthCentre) {
  const Geodetic centre = ecef_to_geodetic({0.0, 0.0, 0.0});
  EXPECT_EQ(centre.latitude_rad, kPi / 2.0);
  EXPECT_EQ(centre.longitude_rad, 0.0);
  EXPECT_EQ(centre.height_km, -kWgs84PolarRadiusKm);
  // Mapping back lands on the centre: x = (N + h) cos(pi/2) with cos(pi/2) = 6e-17 in double.
  expect_vector_near(geodetic_to_ecef(centre), {0.0, 0.0, 0.0}, 1e-12);

  const Geodetic below_centre = ecef_to_geodetic({0.0, 0.0, -10.0});
  EXPECT_EQ(below_centre.latitude_rad, -kPi / 2.0);
  EXPECT_EQ(below_centre.height_km, 10.0 - kWgs84PolarRadiusKm);
}

// atan2(-0.0, x < 0) is -pi; the documented longitude range is (-pi, pi].
TEST(GeodeticTest, LongitudeRangeOnAntimeridian) {
  EXPECT_EQ(ecef_to_geodetic({-7000.0, -0.0, 100.0}).longitude_rad, kPi);
  EXPECT_EQ(ecef_to_geodetic({-7000.0, 0.0, 100.0}).longitude_rad, kPi);
  // A longitude of -180 degrees on input comes back as +180.
  const Geodetic g = ecef_to_geodetic(geodetic_to_ecef(geodetic_deg(20.0, -180.0, 550.0)));
  EXPECT_GT(g.longitude_rad, 0.0);
  EXPECT_NEAR(g.longitude_rad, kPi, 1e-15);
}

// ---------------------------------------------------------------------------------------
// Round trips over a grid: every 7.5 deg of latitude (both poles and the equator
// included), every 15 deg of longitude (both -180 and +180), and heights from -10 km to
// 40,000 km. Required accuracy 1 mm.
// ---------------------------------------------------------------------------------------

TEST(GeodeticTest, RoundTripGrid) {
  constexpr std::array kHeightsKm{-10.0, -0.001, 0.0,     0.5,     340.0,
                                  550.0, 1200.0, 20200.0, 35786.0, 40000.0};
  int checked = 0;
  for (int lat_step = -12; lat_step <= 12; ++lat_step) {
    for (int lon_step = -12; lon_step <= 12; ++lon_step) {
      for (const double height_km : kHeightsKm) {
        const Geodetic g = geodetic_deg(7.5 * lat_step, 15.0 * lon_step, height_km);
        const Eigen::Vector3d ecef = geodetic_to_ecef(g);
        const Geodetic back = ecef_to_geodetic(ecef);
        SCOPED_TRACE(testing::Message() << "lat " << 7.5 * lat_step << " lon " << 15.0 * lon_step
                                        << " h " << height_km);
        expect_geodetic_near(back, g, kRoundTripToleranceKm);
        expect_vector_near(geodetic_to_ecef(back), ecef, kRoundTripToleranceKm);
        EXPECT_GE(back.latitude_rad, -kPi / 2.0);
        EXPECT_LE(back.latitude_rad, kPi / 2.0);
        EXPECT_GT(back.longitude_rad, -kPi);
        EXPECT_LE(back.longitude_rad, kPi);
        ++checked;
      }
    }
  }
  EXPECT_EQ(checked, 25 * 25 * static_cast<int>(kHeightsKm.size()));
}

// The header claims the method holds far below the surface too (outside ~43 km of the
// centre). Checked to the same 1 mm at -1000 km and -6000 km.
TEST(GeodeticTest, RoundTripDeepBelowSurface) {
  for (const double height_km : {-1000.0, -6000.0}) {
    for (const double latitude_deg : {-90.0, -60.0, -1.0, 0.0, 33.0, 89.0, 90.0}) {
      const Geodetic g = geodetic_deg(latitude_deg, 40.0, height_km);
      SCOPED_TRACE(testing::Message() << "lat " << latitude_deg << " h " << height_km);
      const Eigen::Vector3d ecef = geodetic_to_ecef(g);
      expect_vector_near(geodetic_to_ecef(ecef_to_geodetic(ecef)), ecef, kRoundTripToleranceKm);
    }
  }
}

}  // namespace
