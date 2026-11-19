#pragma once

#include "satsim/frames/wgs84.hpp"
#include "satsim/ground/sites.hpp"

namespace satsim::ground {

/// Mean radius of the Earth, km: the IUGG mean radius R1 = (2a + b) / 3 of the WGS-84
/// ellipsoid, about 6371.0088 km. Used only for distances on a sphere; positions use the
/// ellipsoid itself (frames::geodetic_to_ecef).
inline constexpr double kEarthMeanRadiusKm =
    ((2.0 * frames::kWgs84EquatorialRadiusKm) + frames::kWgs84PolarRadiusKm) / 3.0;

/// Great-circle distance, km, between two points given by geodetic latitude and longitude in
/// radians, on a SPHERE of radius kEarthMeanRadiusKm. This is not the ellipsoid geodesic: the
/// two differ by under about 0.5%, well inside the uncertainty of a fibre route. Uses the atan2
/// form, accurate for identical and antipodal points alike. Any finite input; longitudes need not
/// be wrapped.
[[nodiscard]] double great_circle_distance_on_mean_sphere_km(double latitude1_rad,
                                                             double longitude1_rad,
                                                             double latitude2_rad,
                                                             double longitude2_rad) noexcept;

/// The same for two ground sites.
[[nodiscard]] double great_circle_distance_on_mean_sphere_km(const GroundSite& a,
                                                             const GroundSite& b) noexcept;

/// Default fibre route inflation: a fibre path's one-way delay divided by the time light in a
/// vacuum takes along the great circle. 1.98 is a published estimate for the best fibre
/// routes available. It covers both the slower light in glass (refractive index about 1.47)
/// and routes longer than the great circle (1.98 / 1.47 leaves a detour factor of about
/// 1.35). It is an ASSUMPTION of this model, not a measurement of any particular cable; real
/// routes between two cities are often worse.
inline constexpr double kDefaultFibreRouteInflation = 1.98;

/// One-way fibre delay, seconds: distance_km * inflation / c, where c is the speed of light
/// in vacuum (kSpeedOfLightVacuumKmPerS). distance_km is the great-circle distance.
[[nodiscard]] double fibre_delay_s(double distance_km,
                                   double inflation = kDefaultFibreRouteInflation) noexcept;

}  // namespace satsim::ground
