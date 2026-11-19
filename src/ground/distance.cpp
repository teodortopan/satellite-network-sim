#include "satsim/ground/distance.hpp"

#include <cmath>

#include "satsim/core/physical_constants.hpp"

namespace satsim::ground {

double great_circle_distance_on_mean_sphere_km(double latitude1_rad, double longitude1_rad,
                                               double latitude2_rad,
                                               double longitude2_rad) noexcept {
  const double delta_longitude = longitude2_rad - longitude1_rad;
  const double sin_lat1 = std::sin(latitude1_rad);
  const double cos_lat1 = std::cos(latitude1_rad);
  const double sin_lat2 = std::sin(latitude2_rad);
  const double cos_lat2 = std::cos(latitude2_rad);
  const double cos_delta = std::cos(delta_longitude);
  const double sin_half_delta = std::sin(delta_longitude / 2.0);
  // Central angle from its sine (y) and cosine (x): well conditioned everywhere, unlike the
  // arccos form (near 0) or the haversine form (near pi). The second component of y is
  // cos(lat1) sin(lat2) - sin(lat1) cos(lat2) cos(dlon), rewritten with 1 - cos(dlon) =
  // 2 sin^2(dlon / 2) so that it is exactly 0 for identical points instead of a rounding
  // residue.
  const double y = std::hypot(cos_lat2 * std::sin(delta_longitude),
                              std::sin(latitude2_rad - latitude1_rad) +
                                  (2.0 * sin_lat1 * cos_lat2 * sin_half_delta * sin_half_delta));
  const double x = (sin_lat1 * sin_lat2) + (cos_lat1 * cos_lat2 * cos_delta);
  return kEarthMeanRadiusKm * std::atan2(y, x);
}

double great_circle_distance_on_mean_sphere_km(const GroundSite& a, const GroundSite& b) noexcept {
  return great_circle_distance_on_mean_sphere_km(a.latitude_rad, a.longitude_rad, b.latitude_rad,
                                                 b.longitude_rad);
}

double fibre_delay_s(double distance_km, double inflation) noexcept {
  return distance_km * inflation / kSpeedOfLightVacuumKmPerS;
}

}  // namespace satsim::ground
