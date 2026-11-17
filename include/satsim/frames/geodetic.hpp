#pragma once

#include <Eigen/Core>

namespace satsim::frames {

/// A position as geodetic coordinates on the WGS-84 ellipsoid.
struct Geodetic {
  double latitude_rad = 0.0;   ///< Geodetic (ellipsoid-normal) latitude, [-pi/2, pi/2].
  double longitude_rad = 0.0;  ///< East longitude, (-pi, pi].
  double height_km = 0.0;      ///< Height above the ellipsoid along the normal.
};

/// Earth-fixed (ECEF) position in km of a geodetic position. Closed form; any latitude,
/// longitude and height.
[[nodiscard]] Eigen::Vector3d geodetic_to_ecef(const Geodetic& geodetic) noexcept;

/// Geodetic coordinates of an Earth-fixed (ECEF) position in km. Closed form (Vermeille
/// 2004), no iteration: accurate to well under 1 mm for any point outside a region of about
/// 43 km around the Earth's centre, which includes every height from -6000 km up.
///
/// Longitude is 0 on the polar axis, where it is undefined. Inside the central region the
/// ellipsoid normal through a point is not unique and the method does not apply; there the
/// point is treated as lying on the polar axis: latitude +pi/2 (-pi/2 if z < 0), height
/// |z| - b, where b is the polar radius. That is exact on the axis itself. In particular the
/// Earth's centre gives latitude +pi/2, longitude 0, height -b, which geodetic_to_ecef maps
/// back to the centre.
[[nodiscard]] Geodetic ecef_to_geodetic(const Eigen::Vector3d& position_ecef_km) noexcept;

}  // namespace satsim::frames
