#include "satsim/frames/wgs84.hpp"

#include <gtest/gtest.h>

namespace {

using satsim::frames::kWgs84EccentricitySquared;
using satsim::frames::kWgs84EquatorialRadiusKm;
using satsim::frames::kWgs84Flattening;
using satsim::frames::kWgs84PolarRadiusKm;

// Defining parameters: NIMA TR8350.2 (3rd ed., 2000), Table 3.1.
TEST(Wgs84Test, DefiningParameters) {
  EXPECT_EQ(kWgs84EquatorialRadiusKm, 6378.137);
  EXPECT_EQ(1.0 / kWgs84Flattening, 298.257223563);
}

// Derived values published in NIMA TR8350.2, Table 3.3, which rounds them:
// b = 6356752.3142 m (to 0.1 mm, so +-5e-8 km) and e^2 = 6.69437999014e-3 (to 12 significant
// digits, +-5e-15). Cross-check with pyerfa 2.0.1.5 / pyproj 3.8.0 in double precision:
// b = 6356752.314245 m, e^2 = 0.0066943799901413165.
TEST(Wgs84Test, DerivedParameters) {
  EXPECT_NEAR(kWgs84PolarRadiusKm, 6356.7523142, 5e-8);
  EXPECT_NEAR(kWgs84EccentricitySquared, 6.69437999014e-3, 5e-15);
}

}  // namespace
