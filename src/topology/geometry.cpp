#include "satsim/topology/geometry.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/frames/wgs84.hpp"
#include "satsim/ground/sites.hpp"

namespace satsim::topology {

namespace {

constexpr double kRadPerDeg = std::numbers::pi / 180.0;

// FCC order DA 26-36: the mask by satellite height.
constexpr double kLowTierCeilingKm = 400.0;  // Below: 10 deg.
constexpr double kHighTierFloorKm = 500.0;   // Above: 25 deg; [400, 500] is 20 deg.
constexpr double kLowTierMaskRad = 10.0 * kRadPerDeg;
constexpr double kMiddleTierMaskRad = 20.0 * kRadPerDeg;
constexpr double kHighTierMaskRad = 25.0 * kRadPerDeg;

}  // namespace

double line_of_sight_radius_km(double atmosphere_km) noexcept {
  return frames::kWgs84EquatorialRadiusKm + atmosphere_km;
}

double segment_min_radius_km(const Eigen::Vector3d& a_ecef_km,
                             const Eigen::Vector3d& b_ecef_km) noexcept {
  const Eigen::Vector3d d = b_ecef_km - a_ecef_km;
  const double length_squared = d.squaredNorm();
  if (length_squared == 0.0) {
    return a_ecef_km.norm();
  }
  // The point a + t d closest to the origin, t clamped to the segment.
  const double t = std::clamp(-a_ecef_km.dot(d) / length_squared, 0.0, 1.0);
  return (a_ecef_km + t * d).norm();
}

bool has_line_of_sight(const Eigen::Vector3d& a_ecef_km, const Eigen::Vector3d& b_ecef_km,
                       double atmosphere_km) noexcept {
  return segment_min_radius_km(a_ecef_km, b_ecef_km) >= line_of_sight_radius_km(atmosphere_km);
}

double max_link_length_km(double radius_a_km, double radius_b_km, double atmosphere_km) noexcept {
  const double r = line_of_sight_radius_km(atmosphere_km);
  if (radius_a_km <= r || radius_b_km <= r) {
    return 0.0;
  }
  return std::sqrt((radius_a_km - r) * (radius_a_km + r)) +
         std::sqrt((radius_b_km - r) * (radius_b_km + r));
}

SiteGeometry site_geometry(const ground::GroundSite& site) noexcept {
  const double cos_latitude = std::cos(site.latitude_rad);
  return SiteGeometry{
      .position_ecef_km = site.position_ecef_km,
      .up_ecef = {cos_latitude * std::cos(site.longitude_rad),
                  cos_latitude * std::sin(site.longitude_rad), std::sin(site.latitude_rad)},
      .latitude_rad = site.latitude_rad,
  };
}

LookAngle look_angle(const SiteGeometry& site, const Eigen::Vector3d& satellite_ecef_km) noexcept {
  const Eigen::Vector3d d = satellite_ecef_km - site.position_ecef_km;
  const double up = site.up_ecef.dot(d);
  const double horizontal = (d - up * site.up_ecef).norm();
  return LookAngle{.elevation_rad = std::atan2(up, horizontal), .slant_range_km = d.norm()};
}

std::optional<std::string> check_mask_settings(const ElevationMaskSettings& settings) {
  if (settings.min_elevation_rad.has_value()) {
    const double e = *settings.min_elevation_rad;
    if (!std::isfinite(e) || e < 0.0 || e >= std::numbers::pi / 2.0) {
      return "the minimum elevation must be finite and in [0, 90) degrees";
    }
  }
  if (settings.max_ground_range_km.has_value()) {
    const double r = *settings.max_ground_range_km;
    if (!std::isfinite(r) || r <= 0.0) {
      return "the longest ground range must be a finite number of kilometres > 0";
    }
  }
  if (settings.min_elevation_rad.has_value() && settings.max_ground_range_km.has_value()) {
    return "a minimum elevation and a longest ground range cannot both be set";
  }
  if (settings.high_latitude_allowance &&
      (settings.min_elevation_rad.has_value() || settings.max_ground_range_km.has_value())) {
    return "the high-latitude allowance cannot be combined with a mask override";
  }
  return std::nullopt;
}

bool ground_link_allowed(const LookAngle& look, double mask_rad,
                         const ElevationMaskSettings& settings) noexcept {
  if (settings.max_ground_range_km.has_value()) {
    return look.slant_range_km <= *settings.max_ground_range_km;
  }
  return look.elevation_rad >= mask_rad;
}

double elevation_mask_rad(double shell_mean_height_km, double site_latitude_rad,
                          const ElevationMaskSettings& settings) noexcept {
  if (settings.min_elevation_rad.has_value()) {
    return *settings.min_elevation_rad;
  }
  if (settings.high_latitude_allowance &&
      std::abs(site_latitude_rad) > settings.high_latitude_limit_rad) {
    return settings.high_latitude_mask_rad;
  }
  if (shell_mean_height_km < kLowTierCeilingKm) {
    return kLowTierMaskRad;
  }
  if (shell_mean_height_km <= kHighTierFloorKm) {
    return kMiddleTierMaskRad;
  }
  return kHighTierMaskRad;
}

std::vector<std::optional<double>> shell_mean_heights_km(
    const constellation::Classification& classification) {
  std::vector<std::optional<double>> heights(classification.planes.size());
  for (std::size_t shell = 0; shell < classification.planes.size(); ++shell) {
    double sum_km = 0.0;
    std::size_t count = 0;
    for (const constellation::Plane& plane : classification.planes[shell]) {
      for (const std::size_t member : plane.members) {
        const constellation::ClassifiedSatellite& satellite = classification.satellites[member];
        if (satellite.geometry.has_value()) {
          sum_km += satellite.geometry->mean_height_km;
          ++count;
        }
      }
    }
    if (count > 0) {
      heights[shell] = sum_km / static_cast<double>(count);
    }
  }
  return heights;
}

}  // namespace satsim::topology
