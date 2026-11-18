// Physical plausibility of a fixture run over one full orbit: heights, the latitude band each
// inclination allows, and lengths preserved by the frame rotations.

#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "propagate/run.hpp"
#include "satsim/core/angles.hpp"
#include "satsim/orbit/element_selection.hpp"
#include "satsim/orbit/omm_loader.hpp"
#include "test_support.hpp"

namespace {

using satsim::propagate::testing::column;
using satsim::propagate::testing::kFixtureIds;
using satsim::propagate::testing::kFixtureSnapshot;
using satsim::propagate::testing::number;
using satsim::propagate::testing::options;
using satsim::propagate::testing::parse_csv;

Eigen::Vector3d vector_at(const std::vector<std::string>& row, const std::string& prefix,
                          const std::string& unit) {
  return {number(row[column(prefix + "x_" + unit)]), number(row[column(prefix + "y_" + unit)]),
          number(row[column(prefix + "z_" + unit)])};
}

TEST(PropagatePhysicalSanityTest, OneOrbitOfTheFixtureIsPhysicallyPlausible) {
  // 100 minutes every minute: longer than the longest fixture period (94 min), so every
  // satellite passes its highest latitude.
  std::ostringstream csv;
  const auto outcome = satsim::propagate::run(options("2026-11-17T18:00:00", 6000, 60), csv);
  ASSERT_FALSE(outcome.error.has_value()) << outcome.error.value_or("");
  const auto records = parse_csv(csv.str());
  ASSERT_EQ(records.size(), 1 + 101 * kFixtureIds.size());

  const auto standard = satsim::orbit::load_omm_json(kFixtureSnapshot / "gp.json");
  const auto supplemental = satsim::orbit::load_omm_json(kFixtureSnapshot / "supgp.json");
  std::map<std::int64_t, double> inclination_deg;
  for (const auto& s :
       satsim::orbit::select_element_sets(standard.element_sets, supplemental.element_sets)
           .selected) {
    inclination_deg[s.elements.norad_cat_id] = satsim::rad_to_deg(s.elements.inclination_rad);
  }

  // |latitude| can exceed the mean inclination by (a) geodetic minus geocentric latitude,
  // e^2 sin(2 phi) / 2 * R / (R + h) <= 0.19 deg (WGS-84 e^2 = 0.00669, largest at 45 deg),
  // and (b) J2 short-period oscillation of the osculating inclination,
  // about 3/4 J2 (R/a)^2 sin(2i) <= 0.03 deg; polar motion adds under 1e-4 deg. 0.3 deg covers
  // both with room; a wrong frame (e.g. latitude from TEME without GMST) does not change
  // |latitude|, but a geocentric/geodetic or degree/radian mix-up breaks this or the height.
  constexpr double kLatitudeMarginDeg = 0.3;
  // Rotations preserve length. Each frame chain is a few 3x3 products: errors of a few units
  // in the last place of a ~7000 km radius (ulp 9e-13 km).
  constexpr double kRadiusToleranceKm = 2e-11;
  constexpr double kSpeedToleranceKmS = 2e-14;

  std::map<std::int64_t, double> max_abs_latitude_deg;
  for (std::size_t r = 1; r < records.size(); ++r) {
    const auto& row = records[r];
    const auto id = static_cast<std::int64_t>(number(row[column("norad_cat_id")]));
    SCOPED_TRACE(row[column("utc")] + " " + std::to_string(id));
    const double height_km = number(row[column("geodetic_height_km")]);
    EXPECT_GT(height_km, 150.0);
    EXPECT_LT(height_km, 700.0);

    const double latitude_deg = std::abs(number(row[column("geodetic_latitude_deg")]));
    EXPECT_LE(latitude_deg, inclination_deg.at(id) + kLatitudeMarginDeg);
    max_abs_latitude_deg[id] = std::max(max_abs_latitude_deg[id], latitude_deg);
    const double longitude_deg = number(row[column("geodetic_longitude_deg")]);
    EXPECT_GT(longitude_deg, -180.0);
    EXPECT_LE(longitude_deg, 180.0);

    const double teme_radius_km = vector_at(row, "teme_", "km").norm();
    EXPECT_NEAR(vector_at(row, "ecef_", "km").norm(), teme_radius_km, kRadiusToleranceKm);
    EXPECT_NEAR(vector_at(row, "j2000_", "km").norm(), teme_radius_km, kRadiusToleranceKm);
    EXPECT_NEAR(vector_at(row, "j2000_v", "km_s").norm(), vector_at(row, "teme_v", "km_s").norm(),
                kSpeedToleranceKmS);
  }
  // The sweep reaches each inclination, so the bound above is a real check.
  for (const auto& [id, inclination] : inclination_deg) {
    EXPECT_GT(max_abs_latitude_deg[id], inclination - 1.0) << id;
  }
}

}  // namespace
