#pragma once

#include <Eigen/Core>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/ground/sites.hpp"

namespace satsim::topology {

// Link geometry. Every position is Earth-fixed (ITRF), in km, at one instant.

/// Default height of the atmosphere layer a laser beam must clear, km (spec 4.3, 4.5).
inline constexpr double kDefaultAtmosphereKm = 80.0;

/// The radius below which a laser beam may not pass: the WGS-84 equatorial radius
/// (6378.137 km) plus `atmosphere_km`. A sphere, not the ellipsoid: over the poles it is
/// 21 km more conservative than the ellipsoid would be.
[[nodiscard]] double line_of_sight_radius_km(double atmosphere_km = kDefaultAtmosphereKm) noexcept;

/// The smallest distance from the Earth's centre to any point of the segment between `a` and
/// `b` (the true minimum over the segment: the foot of the perpendicular from the centre when
/// it falls inside the segment, else the nearer endpoint). If a == b, |a|.
[[nodiscard]] double segment_min_radius_km(const Eigen::Vector3d& a_ecef_km,
                                           const Eigen::Vector3d& b_ecef_km) noexcept;

/// Whether the segment between two satellites stays at or above line_of_sight_radius_km: a
/// minimum exactly at the limit has line of sight.
[[nodiscard]] bool has_line_of_sight(const Eigen::Vector3d& a_ecef_km,
                                     const Eigen::Vector3d& b_ecef_km,
                                     double atmosphere_km = kDefaultAtmosphereKm) noexcept;

/// The longest link with line of sight between satellites at distances `radius_a_km` and
/// `radius_b_km` from the Earth's centre: the two tangent lengths sqrt(r^2 - R^2) to the
/// sphere of radius R = line_of_sight_radius_km, added. About 4,508 km for two satellites at
/// 462 km height. 0 if either radius is at or below R (such a satellite sees nothing above
/// the layer tangentially; a pair might still see each other, but only at a shorter range,
/// and no reported maximum is meaningful).
[[nodiscard]] double max_link_length_km(double radius_a_km, double radius_b_km,
                                        double atmosphere_km = kDefaultAtmosphereKm) noexcept;

/// A ground site's geometry for elevation: its Earth-fixed position (on the ellipsoid,
/// height 0) and the local vertical, the outward unit normal of the WGS-84 ellipsoid at its
/// geodetic latitude and longitude (cos lat cos lon, cos lat sin lon, sin lat). Not the
/// geocentric direction of the position, which is up to 0.19 deg off the normal at 45 deg
/// latitude.
struct SiteGeometry {
  Eigen::Vector3d position_ecef_km = Eigen::Vector3d::Zero();
  Eigen::Vector3d up_ecef = Eigen::Vector3d::UnitZ();
  double latitude_rad = 0.0;  ///< Geodetic.
};

[[nodiscard]] SiteGeometry site_geometry(const ground::GroundSite& site) noexcept;

/// Elevation and distance of a satellite from a ground site.
struct LookAngle {
  /// Angle above the plane perpendicular to the site's ellipsoid normal, in [-pi/2, pi/2].
  double elevation_rad = 0.0;
  double slant_range_km = 0.0;
};

/// The look angle from `site` to a satellite: elevation = atan2(up . d, |d - (up . d) up|)
/// with d the vector from the site to the satellite (well conditioned at 0 and at 90 deg).
/// Precondition: the satellite is not at the site.
[[nodiscard]] LookAngle look_angle(const SiteGeometry& site,
                                   const Eigen::Vector3d& satellite_ecef_km) noexcept;

/// The elevation mask from FCC order DA 26-36, its option, and the two overrides for
/// published setups (Phase 3 spec 4.4, 4.8), each off by default. At most one override is set, and
/// with an override the high-latitude allowance is off (check_mask_settings).
struct ElevationMaskSettings {
  /// The older allowance: sites strictly above high_latitude_limit_rad (north or south) use
  /// high_latitude_mask_rad for every satellite. Off by default (spec 4.4).
  bool high_latitude_allowance = false;
  double high_latitude_limit_rad = 62.0 * std::numbers::pi / 180.0;
  double high_latitude_mask_rad = 5.0 * std::numbers::pi / 180.0;
  /// One mask for every site and shell, replacing the FCC masks (--min-elevation-deg).
  std::optional<double> min_elevation_rad = std::nullopt;
  /// Hypatia's rule (--max-ground-range-km): a site may use a satellite whose slant range is
  /// at most this, km, with no elevation test at all.
  std::optional<double> max_ground_range_km = std::nullopt;
};

/// nullopt if usable, else what is wrong: min_elevation_rad finite and in [0, pi/2),
/// max_ground_range_km finite and > 0, not both set, and neither with the high-latitude
/// allowance.
[[nodiscard]] std::optional<std::string> check_mask_settings(const ElevationMaskSettings& settings);

/// The lowest elevation at which a site may use a satellite of a shell whose mean height is
/// `shell_mean_height_km` (FCC order DA 26-36):
///   height < 400 km                 10 deg
///   400 km <= height <= 500 km      20 deg  (both 400 and 500 km belong to this tier)
///   height > 500 km                 25 deg
/// With settings.high_latitude_allowance, a site with |latitude| > the limit (strictly) uses
/// the allowance's mask instead, whatever the height. With settings.min_elevation_rad, that
/// angle, whatever the height and latitude. Under settings.max_ground_range_km there is no
/// mask: callers test the range instead (ground_link_allowed).
[[nodiscard]] double elevation_mask_rad(double shell_mean_height_km, double site_latitude_rad,
                                        const ElevationMaskSettings& settings = {}) noexcept;

/// Whether a site may use a satellite with look angle `look`: under the range rule
/// (settings.max_ground_range_km) slant range <= the range, else elevation >= `mask_rad` (the
/// site's elevation_mask_rad for the satellite's shell). Both limits are inclusive.
[[nodiscard]] bool ground_link_allowed(const LookAngle& look, double mask_rad,
                                       const ElevationMaskSettings& settings) noexcept;

/// Per shell of `classification`, the mean of its plane members' mean heights
/// (OrbitGeometry::mean_height_km), km: the height the mask uses. nullopt for a shell with no
/// plane member.
[[nodiscard]] std::vector<std::optional<double>> shell_mean_heights_km(
    const constellation::Classification& classification);

}  // namespace satsim::topology
