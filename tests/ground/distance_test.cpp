#include "satsim/ground/distance.hpp"

#include <gtest/gtest.h>

#include <array>
#include <numbers>
#include <string>

#include "satsim/core/angles.hpp"
#include "satsim/core/physical_constants.hpp"
#include "satsim/ground/sites.hpp"

namespace {

using satsim::deg_to_rad;
using satsim::kSpeedOfLightVacuumKmPerS;
using satsim::ground::fibre_delay_s;
using satsim::ground::great_circle_distance_on_mean_sphere_km;
using satsim::ground::kDefaultFibreRouteInflation;
using satsim::ground::kEarthMeanRadiusKm;
using satsim::ground::make_ground_site;

constexpr double kHalfCircumferenceKm = std::numbers::pi * kEarthMeanRadiusKm;

double distance_deg(double lat1, double lon1, double lat2, double lon2) {
  return great_circle_distance_on_mean_sphere_km(deg_to_rad(lat1), deg_to_rad(lon1),
                                                 deg_to_rad(lat2), deg_to_rad(lon2));
}

TEST(DistanceTest, MeanRadiusIsTheIuggValueForWgs84) {
  // R1 = (2a + b) / 3 = 6371.0088 km (Moritz, Geodetic Reference System 1980).
  EXPECT_NEAR(kEarthMeanRadiusKm, 6371.0088, 1e-4);
}

TEST(DistanceTest, SpeedOfLightIsExact) { EXPECT_EQ(kSpeedOfLightVacuumKmPerS, 299792.458); }

struct PairCase {
  const char* name;
  // Degrees, copied from data/ground/cities.json.
  double lat1;
  double lon1;
  double lat2;
  double lon2;
  double expected_km;
};

// The study pairs of data/ground/cities.json and their distances to the kilometre.
TEST(DistanceTest, StudyPairs) {
  const std::array<PairCase, 6> cases = {{
      {"New York - London", 40.7128, -74.006, 51.5074, -0.1278, 5570.0},
      {"San Francisco - Tokyo", 37.7749, -122.4194, 35.6762, 139.6503, 8275.0},
      {"Frankfurt - Singapore", 50.1109, 8.6821, 1.3521, 103.8198, 10258.0},
      {"Detroit - Buenos Aires", 42.3314, -83.0458, -34.6037, -58.3816, 8916.0},
      {"New York - Chicago", 40.7128, -74.006, 41.8781, -87.6298, 1144.0},
      {"Reykjavik - Vladivostok", 64.1466, -21.9426, 43.1155, 131.8855, 7869.0},
  }};
  for (const PairCase& c : cases) {
    EXPECT_NEAR(distance_deg(c.lat1, c.lon1, c.lat2, c.lon2), c.expected_km, 1.0) << c.name;
    // Symmetric.
    EXPECT_NEAR(distance_deg(c.lat2, c.lon2, c.lat1, c.lon1),
                distance_deg(c.lat1, c.lon1, c.lat2, c.lon2), 1e-9)
        << c.name;
  }
}

TEST(DistanceTest, IdenticalPointsAreZero) {
  EXPECT_EQ(distance_deg(40.7128, -74.006, 40.7128, -74.006), 0.0);
  EXPECT_EQ(distance_deg(0.0, 0.0, 0.0, 0.0), 0.0);
  // The same point with longitudes a full turn apart.
  EXPECT_NEAR(distance_deg(10.0, 180.0, 10.0, -180.0), 0.0, 1e-9);
}

TEST(DistanceTest, AntipodalPointsAreHalfACircumference) {
  EXPECT_NEAR(distance_deg(0.0, 0.0, 0.0, 180.0), kHalfCircumferenceKm, 1e-9);
  EXPECT_NEAR(distance_deg(45.0, 30.0, -45.0, -150.0), kHalfCircumferenceKm, 1e-9);
  EXPECT_NEAR(distance_deg(-34.6037, -58.3816, 34.6037, 121.6184), kHalfCircumferenceKm, 1e-9);
}

TEST(DistanceTest, Poles) {
  EXPECT_NEAR(distance_deg(90.0, 0.0, -90.0, 0.0), kHalfCircumferenceKm, 1e-9);
  // Longitude means nothing at a pole.
  EXPECT_NEAR(distance_deg(90.0, 0.0, 90.0, 123.0), 0.0, 1e-9);
  EXPECT_NEAR(distance_deg(-90.0, -45.0, -90.0, 170.0), 0.0, 1e-9);
  EXPECT_NEAR(distance_deg(90.0, 0.0, 0.0, 77.0), kHalfCircumferenceKm / 2.0, 1e-9);
  EXPECT_NEAR(distance_deg(90.0, 0.0, 60.0, -100.0), kHalfCircumferenceKm / 6.0, 1e-9);
}

TEST(DistanceTest, CrossesTheDateLine) {
  // Two degrees of the equator, not 358.
  EXPECT_NEAR(distance_deg(0.0, 179.0, 0.0, -179.0), kHalfCircumferenceKm / 90.0, 1e-9);
}

TEST(DistanceTest, SiteOverloadUsesTheSiteCoordinates) {
  const auto new_york = make_ground_site("New York", 40.7128, -74.006);
  const auto london = make_ground_site("London", 51.5074, -0.1278);
  EXPECT_EQ(great_circle_distance_on_mean_sphere_km(new_york, london),
            distance_deg(40.7128, -74.006, 51.5074, -0.1278));
}

TEST(FibreDelayTest, IsDistanceTimesInflationOverC) {
  EXPECT_EQ(kDefaultFibreRouteInflation, 1.98);
  // 1000 km * 1.98 / 299792.458 km/s = 6.604569 ms.
  EXPECT_NEAR(fibre_delay_s(1000.0), 6.604569e-3, 1e-9);
  EXPECT_NEAR(fibre_delay_s(1000.0, 1.0), 1000.0 / 299792.458, 1e-15);
  EXPECT_EQ(fibre_delay_s(0.0), 0.0);
  // New York - London: 5570.2 km, about 36.8 ms one way.
  EXPECT_NEAR(fibre_delay_s(distance_deg(40.7128, -74.006, 51.5074, -0.1278)), 36.79e-3, 0.01e-3);
}

}  // namespace
