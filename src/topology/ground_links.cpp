#include "satsim/topology/ground_links.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "satsim/constellation/classification.hpp"
#include "satsim/frames/wgs84.hpp"
#include "satsim/ground/sites.hpp"
#include "satsim/topology/geometry.hpp"
#include "satsim/topology/positions.hpp"

namespace satsim::topology {

namespace {

constexpr double kRadPerDeg = std::numbers::pi / 180.0;

// The largest angle between the WGS-84 ellipsoid normal and the geocentric direction of a
// point on the ellipsoid is 0.1924 deg (at 45 deg latitude); 0.2 deg bounds it.
constexpr double kMaxNormalTiltRad = 0.2 * kRadPerDeg;

// The quick test is made slightly looser than the mask, so that rounding in it can never
// reject a satellite the exact elevation would accept.
constexpr double kQuickTestSlackRad = 1e-6;

// Extra room on the z window for the bound below, km.
constexpr double kWindowMarginKm = 50.0;

// An upper bound on the slant range from any site on the ellipsoid to a satellite at most
// `max_radius_km` from the centre at a (geodetic) elevation of at least `min_mask_rad`. In
// the triangle centre, site, satellite, with site radius rs and geocentric elevation e:
// r^2 = rs^2 + s^2 + 2 rs s sin e, so s = -rs sin e + sqrt(r^2 - rs^2 cos^2 e). Taking e at its
// smallest (the mask less the normal's tilt) and rs at the polar radius (the smallest a site
// has) gives the largest s; a margin covers the remaining approximations.
double max_slant_range_km(double max_radius_km, double min_mask_rad) {
  const double e = min_mask_rad - kMaxNormalTiltRad;
  const double rs = frames::kWgs84PolarRadiusKm;
  const double cos_e = std::cos(e);
  const double under = (max_radius_km * max_radius_km) - (rs * rs * cos_e * cos_e);
  return (-rs * std::sin(e)) + std::sqrt(std::max(under, 0.0)) + kWindowMarginKm;
}

}  // namespace

GroundVisibility::GroundVisibility(std::span<const ground::GroundSite> sites,
                                   const constellation::Classification& classification,
                                   const ElevationMaskSettings& mask_settings)
    : shell_count_(classification.planes.size()) {
  for (std::size_t i = 0; i < classification.satellites.size(); ++i) {
    const constellation::ClassifiedSatellite& s = classification.satellites[i];
    if (std::holds_alternative<constellation::PlaneAssignment>(s.placement) && s.shell_index) {
      members_.push_back(
          Member{.satellite = i, .norad_cat_id = s.norad_cat_id, .shell_index = *s.shell_index});
    }
  }
  const std::vector<std::optional<double>> heights = shell_mean_heights_km(classification);
  sites_.reserve(sites.size());
  masks_rad_.assign(sites.size() * shell_count_, std::numeric_limits<double>::quiet_NaN());
  sin_squared_masks_.assign(sites.size() * shell_count_, 0.0);
  min_mask_rad_.assign(sites.size(), std::numbers::pi / 2.0);
  for (std::size_t site = 0; site < sites.size(); ++site) {
    sites_.push_back(site_geometry(sites[site]));
    for (std::size_t shell = 0; shell < shell_count_; ++shell) {
      const std::optional<double>& height_km = heights[shell];
      if (!height_km.has_value()) {
        continue;
      }
      const double mask = elevation_mask_rad(*height_km, sites[site].latitude_rad, mask_settings);
      const std::size_t k = (site * shell_count_) + shell;
      masks_rad_[k] = mask;
      const double loose = std::sin(std::max(mask - kQuickTestSlackRad, 0.0));
      sin_squared_masks_[k] = loose * loose;
      min_mask_rad_[site] = std::min(min_mask_rad_[site], mask);
    }
  }
}

std::optional<double> GroundVisibility::mask_rad(std::size_t site, std::size_t shell) const {
  if (site >= sites_.size() || shell >= shell_count_) {
    return std::nullopt;
  }
  const double mask = masks_rad_[(site * shell_count_) + shell];
  return std::isnan(mask) ? std::nullopt : std::optional<double>(mask);
}

std::vector<std::vector<VisibleSatellite>> GroundVisibility::at(
    const StepPositions& positions) const {
  // Plane members with a position, sorted by z.
  struct Placed {
    double z_km;
    const Eigen::Vector3d* position;
    std::size_t member;  // Index into members_.
  };
  std::vector<Placed> placed;
  placed.reserve(members_.size());
  double max_radius_km = 0.0;
  for (std::size_t m = 0; m < members_.size(); ++m) {
    const std::size_t satellite = members_[m].satellite;
    if (satellite >= positions.position_ecef_km.size()) {
      continue;
    }
    const std::optional<Eigen::Vector3d>& p = positions.position_ecef_km[satellite];
    if (p.has_value()) {
      placed.push_back(Placed{.z_km = p->z(), .position = &*p, .member = m});
      max_radius_km = std::max(max_radius_km, p->norm());
    }
  }
  std::ranges::sort(placed, [](const Placed& a, const Placed& b) { return a.z_km < b.z_km; });

  std::vector<std::vector<VisibleSatellite>> visible(sites_.size());
  for (std::size_t site = 0; site < sites_.size(); ++site) {
    const SiteGeometry& g = sites_[site];
    const double window_km = max_slant_range_km(max_radius_km, min_mask_rad_[site]);
    const double z = g.position_ecef_km.z();
    const auto first = std::ranges::lower_bound(placed, z - window_km, {}, &Placed::z_km);
    const auto last = std::ranges::upper_bound(placed, z + window_km, {}, &Placed::z_km);
    std::vector<VisibleSatellite>& out = visible[site];
    for (auto it = first; it != last; ++it) {
      const Member& member = members_[it->member];
      const std::size_t k = (site * shell_count_) + member.shell_index;
      const Eigen::Vector3d& p = *it->position;
      const Eigen::Vector3d d = p - g.position_ecef_km;
      const double up = g.up_ecef.dot(d);
      if (up < 0.0 || up * up < d.squaredNorm() * sin_squared_masks_[k]) {
        continue;
      }
      const LookAngle look = look_angle(g, p);
      if (look.elevation_rad >= masks_rad_[k]) {
        out.push_back(VisibleSatellite{.satellite = member.satellite,
                                       .norad_cat_id = member.norad_cat_id,
                                       .shell_index = member.shell_index,
                                       .elevation_rad = look.elevation_rad,
                                       .slant_range_km = look.slant_range_km,
                                       .mask_rad = masks_rad_[k]});
      }
    }
    std::ranges::sort(out, [](const VisibleSatellite& a, const VisibleSatellite& b) {
      return a.satellite < b.satellite;
    });
  }
  return visible;
}

std::optional<VisibleSatellite> pick_serving_satellite(std::span<const VisibleSatellite> visible,
                                                       ServingRule rule) {
  std::optional<VisibleSatellite> best;
  switch (rule) {
    case ServingRule::kHighestElevation:
      for (const VisibleSatellite& v : visible) {
        if (!best || v.elevation_rad > best->elevation_rad ||
            (v.elevation_rad == best->elevation_rad && v.norad_cat_id < best->norad_cat_id)) {
          best = v;
        }
      }
      break;
  }
  return best;
}

}  // namespace satsim::topology
