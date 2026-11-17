#include "satsim/frames/geodetic.hpp"

#include <Eigen/Core>
#include <cmath>
#include <numbers>

#include "satsim/frames/wgs84.hpp"

namespace satsim::frames {

namespace {
constexpr double kA = kWgs84EquatorialRadiusKm;
constexpr double kE2 = kWgs84EccentricitySquared;
constexpr double kE4 = kE2 * kE2;
constexpr double kPi = std::numbers::pi;

// atan2 returns -pi for (y = -0.0, x < 0); the documented range is (-pi, pi].
double longitude_of(double x, double y) noexcept {
  if (x == 0.0 && y == 0.0) {
    return 0.0;  // On the polar axis longitude is undefined; report 0.
  }
  const double longitude = std::atan2(y, x);
  return longitude == -kPi ? kPi : longitude;
}
}  // namespace

Eigen::Vector3d geodetic_to_ecef(const Geodetic& geodetic) noexcept {
  const double sin_lat = std::sin(geodetic.latitude_rad);
  const double cos_lat = std::cos(geodetic.latitude_rad);
  // Radius of curvature in the prime vertical: distance along the normal from the surface
  // to the polar axis.
  const double n = kA / std::sqrt(1.0 - kE2 * sin_lat * sin_lat);
  const double horizontal = (n + geodetic.height_km) * cos_lat;
  return {horizontal * std::cos(geodetic.longitude_rad),
          horizontal * std::sin(geodetic.longitude_rad),
          (n * (1.0 - kE2) + geodetic.height_km) * sin_lat};
}

Geodetic ecef_to_geodetic(const Eigen::Vector3d& position_ecef_km) noexcept {
  // H. Vermeille, "Computing geodetic coordinates from geocentric coordinates",
  // Journal of Geodesy 78 (2004) 94-95. Exact closed form (it solves the quartic for the
  // foot of the normal analytically) for every point outside the evolute of the meridian
  // ellipse; r > 0 below is a sufficient condition for that.
  const double x = position_ecef_km.x();
  const double y = position_ecef_km.y();
  const double z = position_ecef_km.z();
  const double horizontal = std::hypot(x, y);
  const double longitude = longitude_of(x, y);

  const double p = (horizontal / kA) * (horizontal / kA);
  const double q = (1.0 - kE2) * (z / kA) * (z / kA);
  const double r = (p + q - kE4) / 6.0;
  if (r <= 0.0) {
    // Within ~43 km of the centre (p + q <= e^4): treat the point as on the polar axis.
    const double height = std::abs(z) - kWgs84PolarRadiusKm;
    return {.latitude_rad = z < 0.0 ? -kPi / 2.0 : kPi / 2.0,
            .longitude_rad = longitude,
            .height_km = height};
  }

  const double s = kE4 * p * q / (4.0 * r * r * r);
  const double t = std::cbrt(1.0 + s + std::sqrt(s * (2.0 + s)));
  const double u = r * (1.0 + t + 1.0 / t);
  const double v = std::sqrt(u * u + kE4 * q);
  const double w = kE2 * (u + v - q) / (2.0 * v);
  const double k = std::sqrt(u + v + w * w) - w;
  const double d = k * horizontal / (k + kE2);
  const double d_z = std::hypot(d, z);

  // Half-angle form of atan(z / d): well conditioned at the equator and at the poles (d = 0).
  return {.latitude_rad = 2.0 * std::atan2(z, d + d_z),
          .longitude_rad = longitude,
          .height_km = (k + kE2 - 1.0) / k * d_z};
}

}  // namespace satsim::frames
