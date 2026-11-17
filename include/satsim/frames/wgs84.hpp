#pragma once

namespace satsim::frames {

// WGS-84 reference ellipsoid (NIMA TR8350.2, 3rd ed., Table 3.1). Only the two defining
// parameters are given here; everything else is derived from them.

/// Equatorial radius (semi-major axis), km. Defining parameter.
inline constexpr double kWgs84EquatorialRadiusKm = 6378.137;

/// Flattening f = (a - b) / a. Defining parameter.
inline constexpr double kWgs84Flattening = 1.0 / 298.257223563;

/// Polar radius (semi-minor axis) b = a (1 - f), km.
inline constexpr double kWgs84PolarRadiusKm = kWgs84EquatorialRadiusKm * (1.0 - kWgs84Flattening);

/// First eccentricity squared e^2 = f (2 - f).
inline constexpr double kWgs84EccentricitySquared = kWgs84Flattening * (2.0 - kWgs84Flattening);

}  // namespace satsim::frames
